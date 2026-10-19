/* Dynamic areas (RISC OS 3.5 on, so a Risc PC's: roger_config.vidc20):
 * memory a program or module creates by number, which can grow and shrink.
 *
 * OS_DynamicArea (&66), as the PRMs give it:
 *   0 create (R1 the number wanted or -1, R2 the initial size, R3 the base
 *     wanted or -1, R4 flags, R5 the maximum size (-1: as much as there
 *     is), R6 a handler, R7 its workspace, R8 the name -> R1 the number,
 *     R3 the base, R5 the maximum)
 *   1 remove (R1), 2 information (R1 -> R2 size, R3 base, R4 flags, R5
 *     maximum, R6 handler, R7 workspace, R8 name), 3 enumerate (R1 -1 or
 *     the last -> R1 the next, -1 at the end), 4 renumber (R1 -> R2)
 * and OS_ReadDynamicArea / OS_ChangeDynamicArea on these numbers (256 on:
 * RISC OS's own are below). Each area's address space is reserved at its
 * maximum size (at most AREA_MAX) from DA_BASE on; growing zero-fills.
 * The area's handler is called around a change, as RISC OS does: R0 0
 * PreGrow / 1 PostGrow / 2 PreShrink / 3 PostShrink, R3 the change, R4 the
 * size (before for Pre, after for Post), R5 the page size, R12 its
 * workspace, R1/R2 a page block and its number of pages; an error from PreGrow, or a
 * smaller R3 from PreShrink, limits it. */
#include <string.h>
#include "internal.h"

#define DA_BASE  0x02100000u              /* (below the Filer's data at &2F00000) */
#define DA_END   0x02F00000u
#define AREA_MAX 0x00400000u
#define PAGE_BLOCK 0x017FD000u          /* (1K, free in the runtime's workspace) */
#define MAXDA 16
static struct { uint32_t num, base, size, max, flags, handler, ws; char name[32]; } da[MAXDA];
static uint32_t next_num = 256, next_base = DA_BASE;

/* the area's handler for reason, R3 change, R4 size: 0, or the error */
static uint32_t handler(int k, uint32_t reason, uint32_t *change, uint32_t size)
{
    if (!da[k].handler) return 0;
    uint32_t saved[16], f[4] = {NF, ZF, CF, VF}, snpc = npc;
    memcpy(saved, R, sizeof saved);
    /* a page block for the pages that come or go: page number, logical
     * address, physical address (3 words each), in the RMA's spare page */
    uint32_t pages = *change / 0x1000, blk = PAGE_BLOCK;
    if (pages > 0x400 / 12) pages = 0x400 / 12;
    uint32_t first = reason < 2 ? da[k].base + (reason == 0 ? size : size - *change) : da[k].base + size - *change;
    for (uint32_t p = 0; p < pages; p++) {
        WR32(blk + 12 * p, (first - DA_BASE) / 0x1000 + p); WR32(blk + 12 * p + 4, first + 0x1000 * p);
        WR32(blk + 12 * p + 8, first + 0x1000 * p);
    }
    R[0] = reason; R[1] = blk; R[2] = *change / 0x1000; R[3] = *change; R[4] = size; R[5] = 0x1000; R[12] = da[k].ws;
    VF = 0;
    call_code(da[k].handler, MODE_SVC, HANDLER_RET | MODE_SVC);
    uint32_t err = VF ? R[0] : 0;
    if (reason == 2 && !err) *change = R[3];
    memcpy(R, saved, sizeof saved);
    NF = f[0]; ZF = f[1]; CF = f[2]; VF = f[3];
    npc = snpc;
    return err;
}

static int find(uint32_t n)
{
    for (int k = 0; k < MAXDA; k++) if (da[k].num && da[k].num == n) return k;
    return -1;
}

/* OS_ReadDynamicArea (b &5C) or OS_ChangeDynamicArea (&2A) on one of these: 1 if handled */
int dynarea_read_change(uint32_t b)
{
    int k = find(R[0] & ~0x80u);
    if (k < 0) return 0;
    if (b == 0x5C) {
        if (R[0] & 0x80) R[2] = da[k].max;
        R[0] = da[k].base; R[1] = da[k].size;
        return 1;
    }
    int32_t want = (int32_t)da[k].size + (int32_t)R[1];
    if (want < 0) want = 0;
    if ((uint32_t)want > da[k].max) { os_error(0x1C6, "Can't extend dynamic area"); return 1; }
    want = (want + 0xFFF) & ~0xFFF;
    uint32_t old = da[k].size, change = (uint32_t)want > old ? want - old : old - want, e;
    if ((uint32_t)want > old) {
        if ((e = handler(k, 0, &change, old))) { R[0] = e; VF = 1; return 1; }
        memset(MEM + da[k].base + old, 0, want - old);
        da[k].size = want;
        handler(k, 1, &change, want);
    } else if ((uint32_t)want < old) {
        if ((e = handler(k, 2, &change, old))) { R[0] = e; VF = 1; return 1; }
        change &= ~0xFFFu;
        da[k].size = old - change;
        handler(k, 3, &change, da[k].size);
    }
    R[1] = change;
    return 1;
}

int dynarea_swi(uint32_t b)
{
    if (b != 0x66) return 0;
    int k;
    switch (R[0]) {
    case 0: {                                          /* create */
        for (k = 0; k < MAXDA && da[k].num; k++) ;
        uint32_t max = (int32_t)R[5] < 0 || R[5] > AREA_MAX ? AREA_MAX : (R[5] + 0xFFF) & ~0xFFF;
        if (k == MAXDA || next_base + max > DA_END) { os_error(0x1C5, "Not enough memory for a dynamic area"); return 1; }
        uint32_t n = (int32_t)R[1] < 0 ? next_num++ : R[1];
        if (find(n) >= 0) { os_error(0x1C7, "Dynamic area number in use"); return 1; }
        uint32_t size = (R[2] + 0xFFF) & ~0xFFF;
        if (size > max) size = max;
        da[k] = (typeof(da[0])){n, next_base, size, max, R[4], R[6], R[7], {0}};
        if (R[8]) cstr(R[8], da[k].name, 0);
        next_base += max;
        memset(MEM + da[k].base, 0, size);
        R[1] = n; R[3] = da[k].base; R[5] = max;
        ev("OS_DynamicArea_create", "iiii", n, da[k].base, size, da[k].handler);
        return 1;
    }
    case 1:                                            /* remove */
        if ((k = find(R[1])) < 0) { os_error(0x1C8, "Unknown dynamic area"); return 1; }
        da[k].num = 0;
        return 1;
    case 2:                                            /* information */
        if ((k = find(R[1])) < 0) { os_error(0x1C8, "Unknown dynamic area"); return 1; }
        R[2] = da[k].size; R[3] = da[k].base; R[4] = da[k].flags; R[5] = da[k].max;
        R[6] = da[k].handler; R[7] = da[k].ws; R[8] = 0;
        return 1;
    case 3: {                                          /* enumerate (RISC OS's own aren't listed) */
        int from = 0;
        if ((int32_t)R[1] >= 0) { if ((k = find(R[1])) < 0) { R[1] = (uint32_t)-1; return 1; } from = k + 1; }
        for (k = from; k < MAXDA && !da[k].num; k++) ;
        R[1] = k < MAXDA ? da[k].num : (uint32_t)-1;
        return 1;
    }
    case 4:                                            /* renumber */
        if ((k = find(R[1])) < 0) { os_error(0x1C8, "Unknown dynamic area"); return 1; }
        da[k].num = R[2];
        return 1;
    }
    os_error(0x1C9, "Bad OS_DynamicArea reason");
    return 1;
}
