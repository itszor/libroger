/* The Window Manager, as much of it as a game launched from the desktop
 * needs: a task that puts an icon on the icon bar, perhaps loads a window
 * template and a menu, and takes over the screen when its icon is clicked.
 *
 * No desktop is drawn. Wimp_Poll returns the events queued with
 * roger_wimp_event(); when there are none it calls the game's wimp_idle
 * hook, which stands in for the user (click the icon bar icon, choose from
 * a menu, Message_Quit) by queueing some. With still nothing queued and
 * null events masked, the task would wait forever, so the run ends. The
 * Wimp reports itself as version 3.16 (RISC OS 3.1), or 3.50 (RISC OS 3.5)
 * on a Risc PC (roger_config.vidc20).
 *
 * Also the memory calls a Wimp task makes: Wimp_SlotSize (the application
 * slot, which is the RAM limit), and the screen's dynamic area
 * (OS_ReadDynamicArea / OS_ChangeDynamicArea 2). */
#include <string.h>
#include "internal.h"

#define WIMP_VERSION (rg->vidc20 ? 350 : 316)   /* RISC OS 3.5 on a Risc PC, else 3.1 */
#define TASK_HANDLE  0x4B534154u          /* any non-zero handle */
#define SLOT_MAX     (DMA_BUF - LOAD)     /* the application slot can't grow into the runtime's workspace */

/* ---------------------------------------------------------------- memory */
static uint32_t slot;                     /* application slot, bytes from &8000 */
uint32_t ram_limit(void) { return LOAD + (slot ? slot : rg->wimpslot); }

static void slot_size(void)               /* Wimp_SlotSize: R0 current (-1 read), R1 next (-1 read) */
{
    if (!slot) slot = rg->wimpslot;
    if ((int32_t)R[0] >= 0) {
        uint32_t want = (R[0] + 0xFFF) & ~0xFFFu;      /* whole pages */
        if (want > SLOT_MAX) want = SLOT_MAX;
        if (want > slot) memset(MEM + LOAD + slot, 0, want - slot);
        slot = want;
        ev("Wimp_SlotSize", "i", slot);
    }
    R[0] = slot; R[1] = slot; R[2] = SLOT_MAX - slot;
}

/* OS_ReadDynamicArea / OS_ChangeDynamicArea, area 2 (screen) only. The
 * screen's memory ends at &2000000 (unless the game placed it), so it
 * grows downwards into what would be free RAM. */
#define SCREEN_MAX 0x200000u
static int dynamic_area(uint32_t b)
{
    if ((R[0] & ~0x80u) != 2) { VF = 1; return 1; }
    if (b == 0x5C) {                      /* R2 the maximum size only if asked for (bit 7) */
        if (R[0] & 0x80) R[2] = SCREEN_MAX;
        R[0] = screen_base(); R[1] = screen_size();
        return 1;
    }
    int32_t want = (int32_t)screen_size() + (int32_t)R[1];         /* OS_ChangeDynamicArea */
    if (want < 0x14000) want = 0x14000;
    if (want > (int32_t)SCREEN_MAX) want = SCREEN_MAX;
    want = (want + 0xFFF) & ~0xFFF;
    R[1] = (uint32_t)(want > (int32_t)screen_size() ? want - screen_size() : screen_size() - want);
    video_set_screen_size(want);
    ev("OS_ChangeDynamicArea", "ii", 2, want);
    return 1;
}

/* ---------------------------------------------------------------- events */
#define QLEN 16
static struct { uint32_t reason, n, block[64]; } queue[QLEN];
static unsigned qhead, qtail;

void roger_wimp_event(uint32_t reason, const uint32_t *block, int nwords)
{
    if (qtail - qhead == QLEN) return;
    unsigned k = qtail++ % QLEN;
    queue[k].reason = reason;
    queue[k].n = nwords > 64 ? 64 : nwords;
    memcpy(queue[k].block, block, queue[k].n * 4);
}

void roger_wimp_click(int32_t window, int32_t icon, uint32_t buttons)
{
    roger_wimp_event(6, (const uint32_t[]){640, 512, buttons, (uint32_t)window, (uint32_t)icon}, 5);
}

void roger_wimp_menu(const int *items)
{
    uint32_t b[16]; int n = 0;
    while (n < 15 && items[n] >= 0) { b[n] = items[n]; n++; }
    b[n++] = 0xFFFFFFFF;
    roger_wimp_event(9, b, n);
}

void roger_wimp_quit(void)                /* Message_Quit, as the Task Manager sends it */
{
    roger_wimp_event(17, (const uint32_t[]){20, 0, 0, 0, 0}, 5);
}

static void poll(void)                    /* Wimp_Poll: R0 mask, R1 block */
{
    if (qhead == qtail && rg->wimp_idle) rg->wimp_idle();
    if (qhead == qtail) {
        if (R[0] & 1) finish("Wimp_Poll with nothing to do");
        R[0] = 0;                         /* Null_Reason_Code */
        return;
    }
    unsigned k = qhead++ % QLEN;
    memcpy(MEM + R[1], queue[k].block, queue[k].n * 4);
    R[0] = queue[k].reason;
    ev("Wimp_Poll", "i", R[0]);
}

/* ---------------------------------------------------------------- templates
 * A template file: a header (font data offset, 3 words reserved), then an
 * index of (offset, size, type, 12-byte name) entries up to a zero word.
 * An entry is a window block as for Wimp_CreateWindow (88 bytes and 32 per
 * icon) followed by its indirected data, with indirected pointers given as
 * offsets from the entry's start. */
static char template_path[2048];

static int wildmatch(const char *pat, const char *s)
{
    for (; *pat > ' '; pat++, s++) {
        if (*pat == '*') {
            for (const char *t = s; ; t++) { if (wildmatch(pat + 1, t)) return 1; if (*t <= ' ') return 0; }
        }
        if (*s <= ' ') return 0;
        if (*pat != '#' && (*pat | 0x20) != (*s | 0x20)) return 0;
    }
    return *s <= ' ';
}

/* Wimp_LoadTemplate: R1 window buffer (<= 0: just the sizes), R2/R3
 * indirected workspace and its end, R4 font array, R5 name (wildcarded;
 * the name found is written back), R6 index entry to search from. Returns
 * R2 past the indirected data used, R6 the next entry (0: not found). */
static void load_template(void)
{
    FILE *f = fopen(template_path, "rb");
    static uint8_t t[1 << 16];
    size_t n = f ? fread(t, 1, sizeof t, f) : 0;
    if (f) fclose(f);
    char name[16]; int l = 0;
    while (l < 12 && MEM[R[5] + l] > ' ') { name[l] = MEM[R[5] + l]; l++; }
    name[l] = 0;
    for (uint32_t e = 16 + 24 * R[6]; e + 24 <= n && *(uint32_t *)(t + e); e += 24) {
        uint32_t off = *(uint32_t *)(t + e), size = *(uint32_t *)(t + e + 4);
        char ename[13]; memcpy(ename, t + e + 12, 12); ename[12] = 0;
        if (!wildmatch(name, ename) || off + size > n) continue;
        const uint8_t *w = t + off;
        uint32_t nicons = *(uint32_t *)(w + 84), fixed = 88 + 32 * nicons;
        if (fixed > size) fixed = size;
        R[6] = (e - 16) / 24 + 1;
        for (l = 0; l < 12 && ename[l] > ' '; l++) MEM[R[5] + l] = ename[l];
        MEM[R[5] + l] = 13;
        if ((int32_t)R[1] <= 0) { R[1] = fixed; R[2] = size - fixed; return; }
        if (R[2] + (size - fixed) > R[3]) { VF = 1; return; }       /* workspace too small */
        memcpy(MEM + R[1], w, fixed);
        memcpy(MEM + R[2], w + fixed, size - fixed);
        /* indirected title (flags at +56) and icons (each 32 bytes from +88:
         * flags at +16, data at +20): relocate offsets into the workspace */
        for (int k = -1; k < (int)nicons; k++) {
            uint32_t flags = RD32(R[1] + (k < 0 ? 56 : 88 + 32 * k + 16));
            uint32_t data = R[1] + (k < 0 ? 72 : 88 + 32 * k + 20);
            if (!(flags & 0x100)) continue;
            for (int p = 0; p < 2; p++) {
                uint32_t v = RD32(data + 4 * p);
                if (v >= fixed && v < size) WR32(data + 4 * p, R[2] + v - fixed);
            }
        }
        R[2] += size - fixed;
        ev("Wimp_LoadTemplate", "s", ename, l);
        return;
    }
    R[6] = 0;
}

/* ---------------------------------------------------------------- SWIs */
static uint32_t next_window = 0x10000;
static uint32_t next_icon;

int wimp_swi(uint32_t b)
{
    switch (b) {
    case 0x5C: case 0x2A: return dynamic_area(b);
    case 0x400EC: slot_size(); return 1;
    case 0x400C0:                                            /* Wimp_Initialise */
        ev("Wimp_Initialise", "i", R[0]);
        R[0] = WIMP_VERSION; R[1] = TASK_HANDLE;
        return 1;
    case 0x400C7: case 0x400E1: poll(); return 1;           /* Wimp_Poll, Wimp_PollIdle */
    case 0x400C1: R[0] = next_window; next_window += 0x100; return 1;   /* Wimp_CreateWindow */
    case 0x400C2: R[0] = next_icon++; return 1;              /* Wimp_CreateIcon */
    case 0x400C3: case 0x400C4: case 0x400C5: case 0x400C6:  /* Delete/Open/Close window, DeleteIcon */
    case 0x400D2: case 0x400D4: case 0x400D8: case 0x400CD:  /* caret, menu, pointer shape, icon state */
        ev("Wimp", "i", b);
        return 1;
    case 0x400CF:                                            /* Wimp_GetPointerInfo */
        WR32(R[1], 640); WR32(R[1] + 4, 512); WR32(R[1] + 8, 0);
        WR32(R[1] + 12, 0xFFFFFFFF); WR32(R[1] + 16, 0xFFFFFFFF);
        return 1;
    case 0x400D9: {                                          /* Wimp_OpenTemplate */
        char name[1100]; cstr(R[1], name, 1);
        if (!hostpath(name, template_path)) { template_path[0] = 0; VF = 1; }
        return 1;
    }
    case 0x400DA: template_path[0] = 0; return 1;           /* Wimp_CloseTemplate */
    case 0x400DB: load_template(); return 1;
    case 0x400DD: ev("Wimp_CloseDown", ""); return 1;
    case 0x400DF: {                                          /* Wimp_ReportError: R0 error, R1 flags */
        char msg[1100]; cstr(R[0] + 4, msg, 0);
        ev("Wimp_ReportError", "is", RD32(R[0]), msg, (int)strlen(msg));
        if (rg_interactive) fprintf(stderr, "error: %s\n", msg);
        R[1] = 1;                                            /* OK */
        return 1;
    }
    case 0x400E3:                                            /* Wimp_SetMode: a number or a selector */
        if (R[0] < 0x100) { vdu_put(22); vdu_put(R[0]); }
        else video_set_mode(R[0]);
        return 1;
    }
    return 0;
}
