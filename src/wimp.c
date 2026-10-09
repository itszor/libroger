/* The Window Manager, as much of it as a game launched from the desktop
 * needs: a task that puts an icon on the icon bar, perhaps loads a window
 * template and a menu, and takes over the screen when its icon is clicked.
 *
 * No desktop is drawn, but windows and icons are kept as the program
 * defines them (below), so that it reads back what it set. Wimp_Poll
 * returns the events queued with
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

static void wimp_auto_select(uint32_t window, int icon, uint32_t buttons);

void roger_wimp_click(int32_t window, int32_t icon, uint32_t buttons)
{
    wimp_auto_select((uint32_t)window, icon, buttons);
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

/* The data transfer protocol's other end, for saving: a Filer window
 * stands under the pointer while game code has armed it with a destination
 * (roger_wimp_save_to()), as if the user had dragged the save box's icon
 * to a directory. Message_DataSave to it is answered with DataSaveAck
 * giving the full pathname, the program saves there and says so with
 * Message_DataLoad, answered with DataLoadAck. */
#define FILER_WINDOW 0x46494C45u
#define FILER_TASK   0x52454C46u
static char save_to[1024];
static uint32_t msg_ref = 0x100;

void roger_wimp_save_to(const char *path) { snprintf(save_to, sizeof save_to, "%s", path ? path : ""); }

static void filer_message(uint32_t m)
{
    uint32_t action = RD32(m + 16), b[64] = {0};
    if (action == 1 && save_to[0]) {                         /* Message_DataSave */
        memcpy(b, MEM + m, 44);                             /* window, icon, x, y, size, type */
        size_t l = strlen(save_to);
        memcpy((char *)b + 44, save_to, l + 1);
        b[0] = (uint32_t)((44 + l + 1 + 3) & ~3u);
        b[1] = FILER_TASK; b[3] = RD32(m + 8); b[2] = ++msg_ref; b[4] = 2;   /* DataSaveAck */
        roger_wimp_event(17, b, (int)(b[0] / 4));
    } else if (action == 3) {                                /* Message_DataLoad: saved */
        char name[1024];
        cstr(m + 44, name, 0);
        ev("Filer_saved", "s", name, (int)strlen(name));
        memcpy(b, MEM + m, 44);
        b[0] = 44; b[1] = FILER_TASK; b[3] = RD32(m + 8); b[2] = ++msg_ref; b[4] = 4;   /* DataLoadAck */
        roger_wimp_event(17, b, 11);
        save_to[0] = 0;
    }
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

/* ---------------------------------------------------------------- windows
 * Windows and icons as the program defines them, so that what it reads
 * back (window state and information, icon state, extents) is what it
 * set; nothing is drawn. A window is its Wimp_CreateWindow block (88
 * bytes: visible area, scroll offsets, behind, flags, colours, work area
 * extent, title flags, ...) and its icons (32 bytes each: bounding box,
 * flags, data). The icon bar is window -2. */
#define MAXWIN   64
#define MAXICON  128
#define WIN_OPEN 0x10000                  /* window flags bit 16: open */
struct win { uint32_t handle; uint8_t blk[88]; uint8_t icon[MAXICON][32]; uint8_t used[MAXICON]; int nicons; };
static struct win wins[MAXWIN], iconbar = {0xFFFFFFFE};
static uint32_t next_window = 0x10000;

static struct win *win_of(uint32_t h)
{
    if (h == 0xFFFFFFFE || h == 0xFFFFFFFF) return &iconbar;
    for (int k = 0; k < MAXWIN; k++) if (wins[k].handle && wins[k].handle == h) return &wins[k];
    return NULL;
}
static uint32_t w32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void s32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* a window's title, for the event log: text, or indirected (title flags
 * bit 8: +72 the buffer) */
static void title_of(const struct win *w, char *out)
{
    uint32_t flags = w32(w->blk + 56);
    if (flags & 0x100) cstr(w32(w->blk + 72), out, 1);
    else { memcpy(out, w->blk + 72, 12); out[12] = 0; for (int k = 0; k < 12; k++) if ((uint8_t)out[k] < 32) out[k] = 0; }
}

static int add_icon(struct win *w, const uint8_t *blk)
{
    int i;
    for (i = 0; i < MAXICON && w->used[i]; i++) ;
    if (i == MAXICON) return -1;
    memcpy(w->icon[i], blk, 32);
    w->used[i] = 1;
    if (i >= w->nicons) w->nicons = i + 1;
    return i;
}

static void create_window(void)           /* R1 block -> R0 handle */
{
    int k;
    for (k = 0; k < MAXWIN && wins[k].handle; k++) ;
    if (k == MAXWIN) { VF = 1; return; }
    struct win *w = &wins[k];
    memset(w, 0, sizeof *w);
    w->handle = next_window; next_window += 0x100;
    memcpy(w->blk, MEM + R[1], 88);
    s32(w->blk + 28, w32(w->blk + 28) & ~WIN_OPEN);
    uint32_t n = w32(w->blk + 84);
    for (uint32_t i = 0; i < n && i < MAXICON; i++) add_icon(w, MEM + R[1] + 88 + 32 * i);
    char t[1100]; title_of(w, t);
    ev("Wimp_CreateWindow", "is", w->handle, t, (int)strlen(t));
    R[0] = w->handle;
}

/* Wimp_OpenWindow: R1 handle, visible area, scroll x/y, behind */
static void open_window(void)
{
    struct win *w = win_of(RD32(R[1]));
    if (!w || w == &iconbar) { VF = 1; return; }
    int was = w32(w->blk + 28) & WIN_OPEN;
    memcpy(w->blk, MEM + R[1] + 4, 28);
    s32(w->blk + 28, w32(w->blk + 28) | WIN_OPEN);
    if (!was) { char t[1100]; title_of(w, t); ev("Wimp_OpenWindow", "is", w->handle, t, (int)strlen(t)); }
}

/* Wimp_GetWindowState: R1 handle -> +4 visible area, scroll, behind, flags */
static void window_state(uint32_t blk, int outline)
{
    struct win *w = win_of(RD32(blk));
    if (!w || w == &iconbar) { VF = 1; return; }
    if (outline) {                        /* Wimp_GetWindowOutline: the visible area and its borders */
        static const int border[4] = {-2, -42, 42, 42};       /* left/bottom: sides, scroll bars; top: title */
        for (int k = 0; k < 4; k++) WR32(blk + 4 + 4 * k, w32(w->blk + 4 * k) + border[k]);
        return;
    }
    memcpy(MEM + blk + 4, w->blk, 32);
}

/* The window last opened as a menu (Wimp_CreateMenu given a window
 * handle: a dialogue box such as a save box), 0 if none; an icon's
 * indirected text buffer (0 if it has none) - for game code standing in
 * for the user (wimp_idle) */
static uint32_t menu_window;
int32_t roger_wimp_menu_window(void) { return (int32_t)menu_window; }
uint32_t roger_wimp_icon_buffer(int32_t window, int icon)
{
    struct win *w = win_of((uint32_t)window);
    if (!w || icon < 0 || icon >= w->nicons) return 0;
    uint32_t flags = w->icon[icon][16] | w->icon[icon][17] << 8 | w->icon[icon][18] << 16 | (uint32_t)w->icon[icon][19] << 24;
    if (!(flags & 0x100)) return 0;
    return w->icon[icon][20] | w->icon[icon][21] << 8 | w->icon[icon][22] << 16 | (uint32_t)w->icon[icon][23] << 24;
}

/* What the Wimp does itself when an icon is clicked: a radio icon (button
 * type 11) is selected by Select - the others in its exclusive selection
 * group deselected; in group 0 it toggles - and toggled by Adjust */
static void wimp_auto_select(uint32_t window, int icon, uint32_t buttons)
{
    struct win *w = win_of(window);
    if (!w || icon < 0 || icon >= w->nicons) return;
    uint32_t f = w32(w->icon[icon] + 16);
    if (((f >> 12) & 15) != 11) return;
    uint32_t esg = (f >> 16) & 31, sel = 1u << 21;
    if ((buttons & 4) && esg) {
        for (int k = 0; k < w->nicons; k++) {
            uint32_t g = w32(w->icon[k] + 16);
            if (k != icon && ((g >> 16) & 31) == esg) s32(w->icon[k] + 16, g & ~sel);
        }
        s32(w->icon[icon] + 16, f | sel);
    } else if (buttons & 5) s32(w->icon[icon] + 16, f ^ sel);
}

static struct win *icon_of(uint32_t h, uint32_t i)
{
    struct win *w = win_of(h);
    return w && i < MAXICON && w->used[i] ? w : NULL;
}

/* Wimp_WhichIcon: R0 window, R1 list, R2 mask, R3 settings -> the icons
 * whose flags & mask == settings, then -1 */
static void which_icon(void)
{
    struct win *w = win_of(R[0]);
    uint32_t o = R[1];
    for (int i = 0; w && i < w->nicons; i++)
        if (w->used[i] && (w32(w->icon[i] + 16) & R[2]) == (R[3] & R[2])) { WR32(o, i); o += 4; }
    WR32(o, 0xFFFFFFFF);
}

/* ---------------------------------------------------------------- SWIs */
static uint32_t caret[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF};

int wimp_swi(uint32_t b)
{
    switch (b) {
    case 0x400C1: create_window(); return 1;                /* Wimp_CreateWindow */
    case 0x400C2: {                                         /* Wimp_CreateIcon: R1 window, icon block */
        struct win *w = win_of(RD32(R[1]));
        int i = w ? add_icon(w, MEM + R[1] + 4) : -1;
        if (i < 0) { VF = 1; return 1; }
        if (w == &iconbar) ev("Wimp_CreateIcon", "ii", RD32(R[1]), i);   /* on the icon bar */
        R[0] = i;
        return 1;
    }
    case 0x400C3: {                                         /* Wimp_DeleteWindow: R1 -> handle */
        struct win *w = win_of(RD32(R[1]));
        if (w && w != &iconbar) w->handle = 0;
        return 1;
    }
    case 0x400C4: {                                         /* Wimp_DeleteIcon: R1 -> window, icon */
        struct win *w = icon_of(RD32(R[1]), RD32(R[1] + 4));
        if (w) w->used[RD32(R[1] + 4)] = 0;
        return 1;
    }
    case 0x400C5: open_window(); return 1;
    case 0x400C6: {                                         /* Wimp_CloseWindow: R1 -> handle */
        struct win *w = win_of(RD32(R[1]));
        if (w && w != &iconbar && (w32(w->blk + 28) & WIN_OPEN)) {
            s32(w->blk + 28, w32(w->blk + 28) & ~WIN_OPEN);
            char t[1100]; title_of(w, t);
            ev("Wimp_CloseWindow", "is", w->handle, t, (int)strlen(t));
        }
        return 1;
    }
    case 0x400C8: case 0x400C9: case 0x400CA:              /* Redraw/UpdateWindow, GetRectangle: */
        R[0] = 0;                                           /* nothing to draw */
        return 1;
    case 0x400CB: window_state(R[1], 0); return 1;         /* Wimp_GetWindowState */
    case 0x400E0: window_state(R[1], 1); return 1;         /* Wimp_GetWindowOutline */
    case 0x400CC: {                                         /* Wimp_GetWindowInfo: R1 -> handle */
        struct win *w = win_of(RD32(R[1]));
        if (!w || w == &iconbar) { VF = 1; return 1; }
        memcpy(MEM + R[1] + 4, w->blk, 88);
        WR32(R[1] + 4 + 84, w->nicons);
        if (!(R[1] & 1))                                    /* bit 0 set: the header only */
            for (int i = 0; i < w->nicons; i++) memcpy(MEM + R[1] + 92 + 32 * i, w->icon[i], 32);
        return 1;
    }
    case 0x400CD: {                                         /* Wimp_SetIconState: R1 -> window, icon, EOR, clear */
        uint32_t h = RD32(R[1]), i = RD32(R[1] + 4);
        struct win *w = icon_of(h, i);
        if (w) s32(w->icon[i] + 16, (w32(w->icon[i] + 16) & ~RD32(R[1] + 12)) ^ RD32(R[1] + 8));
        return 1;
    }
    case 0x400CE: {                                         /* Wimp_GetIconState: R1 -> window, icon; +8 the icon */
        uint32_t h = RD32(R[1]), i = RD32(R[1] + 4);
        struct win *w = icon_of(h, i);
        if (!w) { VF = 1; return 1; }
        memcpy(MEM + R[1] + 8, w->icon[i], 32);
        return 1;
    }
    case 0x400D6: which_icon(); return 1;
    case 0x400D7: {                                         /* Wimp_SetExtent: R0 window, R1 work area extent */
        struct win *w = win_of(R[0]);
        if (!w || w == &iconbar) { VF = 1; return 1; }
        memcpy(w->blk + 40, MEM + R[1], 16);
        return 1;
    }
    case 0x400D2:                                           /* Wimp_SetCaretPosition: R0-R5 */
        for (int k = 0; k < 6; k++) caret[k] = R[k];
        return 1;
    case 0x400D3:                                           /* Wimp_GetCaretPosition: R1 block */
        for (int k = 0; k < 6; k++) WR32(R[1] + 4 * k, caret[k]);
        return 1;
    case 0x400D1: case 0x400D0: case 0x400DC: case 0x400E6: /* ForceRedraw, DragBox, ProcessKey, SetColour, */
    case 0x400F0: case 0x400E8: case 0x400D8:              /* TextColour, CreateSubMenu, SetPointerShape */
        return 1;
    case 0x400E5:                                           /* Wimp_ReadPalette: R1 20 words (&BBGGRR00) */
        for (int k = 0; k < 20; k++) {
            uint32_t v = vidc_palette[k & 15];
            WR32(R[1] + 4 * k, (v & 0xF) * 0x11 << 8 | (v >> 4 & 0xF) * 0x11 << 16 | (v >> 8 & 0xF) * 0x11u << 24);
        }
        return 1;
    case 0x400E7: {                                         /* Wimp_SendMessage: R0 reason, R1 block, R2 task */
        ev("Wimp_SendMessage", "iii", R[0], RD32(R[1] + 16), R[2]);
        WR32(R[1] + 4, TASK_HANDLE);
        if (R[0] >= 17 && R[0] <= 19) WR32(R[1] + 8, ++msg_ref);
        if (R[2] == TASK_HANDLE) roger_wimp_event(R[0], (uint32_t *)(MEM + R[1]), RD32(R[1]) / 4);
        else if (R[2] == FILER_WINDOW || R[2] == FILER_TASK) filer_message(R[1]);
        return 1;
    }
    case 0x400F1:                                           /* Wimp_TransferBlock: within this task */
        memmove(MEM + R[3], MEM + R[1], R[4]);
        return 1;
    case 0x5C: case 0x2A: return dynamic_area(b);
    case 0x400EC: slot_size(); return 1;
    case 0x400C0:                                            /* Wimp_Initialise */
        ev("Wimp_Initialise", "i", R[0]);
        R[0] = WIMP_VERSION; R[1] = TASK_HANDLE;
        return 1;
    case 0x400C7: case 0x400E1: poll(); return 1;           /* Wimp_Poll, Wimp_PollIdle */
    case 0x400D4:                                            /* Wimp_CreateMenu */
    {
        ev("Wimp_CreateMenu", "iii", R[1], R[2], R[3]);
        struct win *old = menu_window ? win_of(menu_window) : NULL, *w = win_of(R[1]);
        if (old) s32(old->blk + 28, w32(old->blk + 28) & ~WIN_OPEN);   /* a new menu closes the old */
        menu_window = 0;
        if (w && w != &iconbar) {                            /* a dialogue box, opened as a menu */
            int32_t x0 = (int32_t)w32(w->blk), y1 = (int32_t)w32(w->blk + 12);
            int32_t wd = (int32_t)w32(w->blk + 8) - x0, ht = y1 - (int32_t)w32(w->blk + 4);
            s32(w->blk, R[2]); s32(w->blk + 12, R[3]);         /* top left at R2, R3 */
            s32(w->blk + 8, R[2] + wd); s32(w->blk + 4, R[3] - ht);
            s32(w->blk + 28, w32(w->blk + 28) | WIN_OPEN);
            menu_window = R[1];
        }
        return 1;
    }
    case 0x400CF:                                            /* Wimp_GetPointerInfo */
        WR32(R[1], 640); WR32(R[1] + 4, 512); WR32(R[1] + 8, 0);
        WR32(R[1] + 12, save_to[0] ? FILER_WINDOW : 0xFFFFFFFF);   /* over the Filer's window, */
        WR32(R[1] + 16, 0xFFFFFFFF);                         /* when a save is to be dropped there */
        return 1;
    case 0x400D9: {                                          /* Wimp_OpenTemplate */
        char name[1100]; cstr(R[1], name, 1);
        if (!hostpath(name, template_path)) { template_path[0] = 0; VF = 1; }
        return 1;
    }
    case 0x400DA: template_path[0] = 0; return 1;           /* Wimp_CloseTemplate */
    case 0x400DB: load_template(); return 1;
    case 0x400DD: ev("Wimp_CloseDown", ""); return 1;
    case 0x400E9:                                            /* Wimp_SpriteOp: by name, on its pool */
        R[0] = (R[0] & 0xFF) | 0x100;
        R[1] = wimp_sprites();
        sprite_op();
        return 1;
    case 0x400F2:                                            /* Wimp_ReadSysInfo */
        switch (R[0]) {
        case 0: R[0] = 1; break;                             /* tasks active */
        case 1: R[0] = screen_mode; break;                   /* the Wimp's mode */
        case 2: {                                            /* the sprite suffix for the mode */
            const struct rg_mode *m = cur_mode();
            R[0] = roger_scratch(m->yeig == 1 ? (m->bpp == 1 ? "23" : "22") : "");
            break;
        }
        case 7: R[0] = WIMP_VERSION; break;
        default: ev("Wimp_ReadSysInfo", "i", R[0]); R[0] = 0; break;
        }
        return 1;
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
