/* OS_Heap (&1D): heaps in memory the caller gives, in RISC OS's format (the
 * PRMs), so that a program that looks inside one finds what it expects:
 *   +0 "Heap", +4 offset from +4 to the first free block (0: none), +8
 *   offset of the first unused byte, +12 the heap's size
 *   a used block: its size (including this word), then the data
 *   a free block: offset to the next free block (from here; 0: last), size
 * Blocks are multiples of 8 bytes. Reasons: 0 initialise (R1 heap, R3
 * size), 1 describe (-> R2 largest free, R3 total free), 2 claim (R3 size
 * -> R2 block), 3 free (R2), 4 extend a block (R2, R3 change -> R2), 5
 * extend the heap (R3 change), 6 read a block's size (R2 -> R3). */
#include <string.h>
#include "internal.h"

#define MAGIC 0x70616548u                 /* "Heap" */

static uint32_t hfree(uint32_t h) { return RD32(h + 4) ? h + 4 + RD32(h + 4) : 0; }
static void set_free(uint32_t h, uint32_t b) { WR32(h + 4, b ? b - (h + 4) : 0); }
static uint32_t nfree(uint32_t b) { return RD32(b) ? b + RD32(b) : 0; }
static void set_next(uint32_t b, uint32_t n) { WR32(b, n ? n - b : 0); }

static int bad(uint32_t h)
{
    if (RD32(h) == MAGIC) return 0;
    os_error(0x1C3, "Not a heap");
    return 1;
}

/* the free blocks, in address order, as a host list; written back after */
#define MAXF 4096
static uint32_t fa[MAXF], fn[MAXF];
static int nf;

static void load(uint32_t h)
{
    nf = 0;
    for (uint32_t b = hfree(h); b && nf < MAXF; b = nfree(b)) { fa[nf] = b; fn[nf++] = RD32(b + 4); }
}
static void store(uint32_t h)
{
    uint32_t hwm = h + RD32(h + 8);
    /* a free block at the top of what's used gives its space back */
    while (nf && fa[nf - 1] + fn[nf - 1] == hwm) { hwm = fa[--nf]; }
    WR32(h + 8, hwm - h);
    set_free(h, nf ? fa[0] : 0);
    for (int k = 0; k < nf; k++) { set_next(fa[k], k + 1 < nf ? fa[k + 1] : 0); WR32(fa[k] + 4, fn[k]); }
}

static uint32_t claim(uint32_t h, uint32_t size)
{
    uint32_t need = (size + 4 + 7) & ~7u;
    load(h);
    for (int k = 0; k < nf; k++)
        if (fn[k] >= need) {
            uint32_t b = fa[k];
            if (fn[k] - need >= 8) { fa[k] += need; fn[k] -= need; }
            else { need = fn[k]; memmove(fa + k, fa + k + 1, (nf - k - 1) * 4); memmove(fn + k, fn + k + 1, (nf - k - 1) * 4); nf--; }
            store(h);
            WR32(b, need);
            return b + 4;
        }
    uint32_t hwm = h + RD32(h + 8);
    if (hwm + need > h + RD32(h + 12)) return 0;
    WR32(h + 8, hwm + need - h);
    WR32(hwm, need);
    return hwm + 4;
}

static void release(uint32_t h, uint32_t blk)
{
    uint32_t b = blk - 4, n = RD32(b);
    load(h);
    int k = 0;
    while (k < nf && fa[k] < b) k++;
    if (k < MAXF) {
        memmove(fa + k + 1, fa + k, (nf - k) * 4); memmove(fn + k + 1, fn + k, (nf - k) * 4);
        fa[k] = b; fn[k] = n; nf++;
        if (k + 1 < nf && fa[k] + fn[k] == fa[k + 1]) {          /* join the next */
            fn[k] += fn[k + 1]; nf--;
            memmove(fa + k + 1, fa + k + 2, (nf - k - 1) * 4); memmove(fn + k + 1, fn + k + 2, (nf - k - 1) * 4);
        }
        if (k > 0 && fa[k - 1] + fn[k - 1] == fa[k]) {           /* and the one before */
            fn[k - 1] += fn[k]; nf--;
            memmove(fa + k, fa + k + 1, (nf - k) * 4); memmove(fn + k, fn + k + 1, (nf - k) * 4);
        }
    }
    store(h);
}

int heap_swi(uint32_t b)
{
    if (b != 0x1D) return 0;
    uint32_t h = R[1];
    switch (R[0]) {
    case 0:                                            /* initialise */
        WR32(h, MAGIC); WR32(h + 4, 0); WR32(h + 8, 16); WR32(h + 12, R[3] & ~7u);
        return 1;
    case 1: {                                          /* describe */
        if (bad(h)) return 1;
        load(h);
        uint32_t big = RD32(h + 12) - RD32(h + 8), total = big;
        for (int k = 0; k < nf; k++) { total += fn[k]; if (fn[k] > big) big = fn[k]; }
        R[2] = big > 4 ? big - 4 : 0; R[3] = total;
        return 1;
    }
    case 2:                                            /* claim */
        if (bad(h)) return 1;
        if (!(R[2] = claim(h, R[3]))) os_error(0x184, "Heap full");
        return 1;
    case 3:                                            /* free */
        if (bad(h)) return 1;
        release(h, R[2]);
        return 1;
    case 4: {                                          /* extend a block */
        if (bad(h)) return 1;
        uint32_t old = RD32(R[2] - 4) - 4, want = (int32_t)(old + R[3]) > 0 ? old + R[3] : 0;
        if (!want) { release(h, R[2]); R[2] = (uint32_t)-1; return 1; }
        if (((want + 4 + 7) & ~7u) <= RD32(R[2] - 4)) return 1;
        uint32_t nb = claim(h, want);
        if (!nb) { os_error(0x184, "Heap full"); return 1; }
        memmove(MEM + nb, MEM + R[2], old);
        release(h, R[2]);
        R[2] = nb;
        return 1;
    }
    case 5:                                            /* extend the heap */
        if (bad(h)) return 1;
        WR32(h + 12, RD32(h + 12) + (R[3] & ~7u));
        return 1;
    case 6:                                            /* read a block's size */
        if (bad(h)) return 1;
        R[3] = RD32(R[2] - 4) - 4;
        return 1;
    }
    os_error(0x1C4, "Bad OS_Heap reason");
    return 1;
}
