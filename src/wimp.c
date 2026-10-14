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
#include <stdlib.h>
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

static uint32_t msg_ref = 0x100;
uint32_t wimp_new_ref(void) { return ++msg_ref; }

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

/* A string in a template's indirected data: its length to the terminator
 * (a control character) inclusive; 0 if off isn't in the data */
static uint32_t template_str(const uint8_t *w, uint32_t fixed, uint32_t size, uint32_t off)
{
    if (off < fixed || off >= size) return 0;
    uint32_t n = 0;
    while (off + n < size && w[off + n] >= 32) n++;
    return off + n < size ? n + 1 : n;
}

/* Lay out a template's indirected data at ws, pointing the window block at
 * blk to it (blk 0: just measure); returns its size. Indirected title
 * (flags at +56, data at +72) and icons (32 bytes each from +88: flags at
 * +16, data at +20): each text gets a buffer of the length its block gives
 * - a template file holds just the strings, packed, but a program may use
 * the whole buffer (Elite's Preferences end it with a terminator, which
 * packed would land in the next icon's validation string) - then its
 * validation string. */
static uint32_t template_data(const uint8_t *w, uint32_t fixed, uint32_t size, uint32_t nicons,
                              uint32_t blk, uint32_t ws)
{
    uint32_t used = 0;
    for (int k = -1; k < (int)nicons; k++) {
        uint32_t fo = k < 0 ? 56 : 88 + 32 * k + 16, d = k < 0 ? 72 : 88 + 32 * k + 20;
        uint32_t flags = *(const uint32_t *)(w + fo);
        if (!(flags & 0x100) || d + 12 > fixed) continue;
        uint32_t text = *(const uint32_t *)(w + d), val = *(const uint32_t *)(w + d + 4);
        uint32_t len = *(const uint32_t *)(w + d + 8);
        uint32_t tn = template_str(w, fixed, size, text);
        if (tn) {
            uint32_t bn = len > tn && len < 0x10000 ? len : tn;
            if (blk) {
                memcpy(MEM + ws + used, w + text, tn);
                memset(MEM + ws + used + tn, 0, bn - tn);
                WR32(blk + d, ws + used);
            }
            used += bn;
        }
        uint32_t vn = (flags & 1) ? template_str(w, fixed, size, val) : 0;   /* text: validation */
        if (vn) {
            if (blk) { memcpy(MEM + ws + used, w + val, vn); WR32(blk + d + 4, ws + used); }
            used += vn;
        }
    }
    return (used + 3) & ~3u;
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
        uint32_t need = template_data(w, fixed, size, nicons, 0, 0);
        if ((int32_t)R[1] <= 0) { R[1] = fixed; R[2] = need; return; }
        if (R[2] + need > R[3]) { VF = 1; return; }                 /* workspace too small */
        memcpy(MEM + R[1], w, fixed);
        R[2] += template_data(w, fixed, size, nicons, R[1], R[2]);
        ev("Wimp_LoadTemplate", "s", ename, l);
        return;
    }
    R[6] = 0;
}

/* ---------------------------------------------------------------- windows
 * Windows and icons as the program defines them (struct win, internal.h),
 * so that what it reads back (window state and information, icon state,
 * extents) is what it set, and so that the desktop (--desktop) can draw
 * them; without it nothing is drawn. Open windows are kept in a stack,
 * bottom to top. */
static struct win wins[WIMP_MAXWIN];
uint32_t wimp_menu_win;                   /* (below) */
#define menu_window wimp_menu_win
struct win wimp_iconbar = {0xFFFFFFFE};
int8_t wimp_bar_side[WIMP_MAXICON];
static uint32_t next_window = 0x10000;
static struct win *stack[WIMP_MAXWIN];
static int nstack;

struct win *wimp_window(uint32_t h)
{
    if (h == 0xFFFFFFFE || h == 0xFFFFFFFF) return &wimp_iconbar;
    for (int k = 0; k < WIMP_MAXWIN; k++) if (wins[k].handle && wins[k].handle == h) return &wins[k];
    return NULL;
}
#define win_of wimp_window
#define iconbar wimp_iconbar
static uint32_t w32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void s32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
uint32_t wimp_rd(const uint8_t *p) { return w32(p); }

int wimp_stack(struct win **out)
{
    memcpy(out, stack, nstack * sizeof *stack);
    return nstack;
}
static void unstack(struct win *w)
{
    for (int k = 0; k < nstack; k++)
        if (stack[k] == w) { memmove(stack + k, stack + k + 1, (--nstack - k) * sizeof *stack); return; }
}

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
    for (i = 0; i < WIMP_MAXICON && w->used[i]; i++) ;
    if (i == WIMP_MAXICON) return -1;
    memcpy(w->icon[i], blk, 32);
    w->used[i] = 1;
    if (i >= w->nicons) w->nicons = i + 1;
    return i;
}

int wimp_bar_icon(const uint8_t *blk, int side)
{
    int i = add_icon(&iconbar, blk);
    if (i >= 0) wimp_bar_side[i] = (int8_t)side;
    return i;
}

struct win *wimp_new_window(const uint8_t *blk)
{
    int k;
    for (k = 0; k < WIMP_MAXWIN && wins[k].handle; k++) ;
    if (k == WIMP_MAXWIN) return NULL;
    struct win *w = &wins[k];
    free(w->back);
    memset(w, 0, sizeof *w);
    w->handle = next_window; next_window += 0x100;
    memcpy(w->blk, blk, 88);
    s32(w->blk + 28, w32(w->blk + 28) & ~WIN_OPEN);
    return w;
}

void wimp_delete(struct win *w)
{
    if (rg_desktop) wimp_close(w);
    if (menu_window == w->handle) menu_window = 0;
    w->handle = 0;
}

static void create_window(void)           /* R1 block -> R0 handle */
{
    struct win *w = wimp_new_window(MEM + R[1]);
    if (!w) { os_error(0, "Too many windows"); return; }
    uint32_t n = w32(w->blk + 84);
    for (uint32_t i = 0; i < n && i < WIMP_MAXICON; i++) add_icon(w, MEM + R[1] + 88 + 32 * i);
    char t[1100]; title_of(w, t);
    ev("Wimp_CreateWindow", "is", w->handle, t, (int)strlen(t));
    R[0] = w->handle;
}

/* Open w at the visible area and scroll offsets given (24 bytes), behind
 * the window `behind` (-1 on top, -2 at the bottom). On the desktop, a
 * window the program draws itself (not auto-redraw: flags bit 4) is asked
 * to redraw when its picture no longer fits. */
void wimp_open(struct win *w, const uint8_t *open, uint32_t behind)
{
    int was = w32(w->blk + 28) & WIN_OPEN;
    memcpy(w->blk, open, 24);
    s32(w->blk + 24, behind);
    s32(w->blk + 28, w32(w->blk + 28) | WIN_OPEN);
    if (!was) { char t[1100]; title_of(w, t); ev("Wimp_OpenWindow", "is", w->handle, t, (int)strlen(t)); }
    if (!rg_desktop) return;
    if (!(w32(w->blk + 28) & 0x10) && memcmp(w->back_vis, w->blk, 24)) w->redraw = 1;
    struct win *over = behind == 0xFFFFFFFE ? NULL : win_of(behind);
    if (was && behind == 0xFFFFFFFD) return;                 /* -3: where it is (RISC OS 3.5) */
    unstack(w);
    int at = nstack;                                         /* -1 (or unknown): on top */
    if (behind == 0xFFFFFFFE) at = 0;
    else if (over && over != w)
        for (int k = 0; k < nstack; k++) if (stack[k] == over) at = k;
    memmove(stack + at + 1, stack + at, (nstack - at) * sizeof *stack);
    stack[at] = w; nstack++;
}

void wimp_close(struct win *w)
{
    if (!(w32(w->blk + 28) & WIN_OPEN)) return;
    s32(w->blk + 28, w32(w->blk + 28) & ~WIN_OPEN);
    char t[1100]; title_of(w, t);
    ev("Wimp_CloseWindow", "is", w->handle, t, (int)strlen(t));
    unstack(w);
    w->redraw = 0;
    if (rg_desktop) desktop_window_gone(w);
}

/* Wimp_OpenWindow: R1 handle, visible area, scroll x/y, behind */
static void open_window(void)
{
    struct win *w = win_of(RD32(R[1]));
    if (!w || w == &iconbar) { VF = 1; return; }
    wimp_open(w, MEM + R[1] + 4, RD32(R[1] + 28));
}

/* Wimp_GetWindowState: R1 handle -> +4 visible area, scroll, behind, flags */
static void window_state(uint32_t blk, int outline)
{
    struct win *w = win_of(RD32(blk));
    if (!w || w == &iconbar) { VF = 1; return; }
    if (outline) {                        /* Wimp_GetWindowOutline: the visible area and its borders */
        if (rg_desktop) {                 /* as the desktop draws it */
            int32_t o[4];
            desktop_outline(w, o);
            for (int k = 0; k < 4; k++) WR32(blk + 4 + 4 * k, o[k]);
            return;
        }
        static const int border[4] = {-2, -42, 42, 42};       /* left/bottom: sides, scroll bars; top: title */
        for (int k = 0; k < 4; k++) WR32(blk + 4 + 4 * k, w32(w->blk + 4 * k) + border[k]);
        return;
    }
    memcpy(MEM + blk + 4, w->blk, 32);
    if (rg_desktop) {                                 /* behind: the window above it, -1 on top */
        uint32_t above = 0xFFFFFFFF;
        for (int k = 0; k + 1 < nstack; k++) if (stack[k] == w) above = stack[k + 1]->handle;
        WR32(blk + 28, above);
        uint32_t f = w32(w->blk + 28) & ~(1u << 20);
        if (wimp_caret[0] == w->handle) f |= 1u << 20;   /* has the input focus */
        WR32(blk + 32, f);
    }
}

/* The window last opened as a menu (Wimp_CreateMenu given a window
 * handle: a dialogue box such as a save box), 0 if none; an icon's
 * indirected text buffer (0 if it has none) - for game code standing in
 * for the user (wimp_idle) */
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
void wimp_auto_select(uint32_t window, int icon, uint32_t buttons)
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
    return w && i < WIMP_MAXICON && w->used[i] ? w : NULL;
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

/* ---------------------------------------------------------------- the caret
 * On the desktop, the window losing the input focus is told (Lose_Caret)
 * and the one gaining it (Gain_Caret) */
uint32_t wimp_caret[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF};
#define caret wimp_caret

void wimp_set_caret(const uint32_t c[6])
{
    uint32_t old[6];
    memcpy(old, caret, sizeof old);
    memcpy(caret, c, sizeof caret);
    if (!rg_desktop || old[0] == c[0]) return;
    if (old[0] != 0xFFFFFFFF && win_of(old[0])) roger_wimp_event(11, old, 6);
    if (c[0] != 0xFFFFFFFF) roger_wimp_event(12, c, 6);
}

/* Wimp_SetCaretPosition: R0 window, R1 icon, R2/R3 offsets, R4 height
 * (-1: from the icon), R5 index (-1: from the x offset). In an icon, the
 * desktop places it at the index (text cells are 16 OS units). */
static void set_caret(void)
{
    uint32_t c[6] = {R[0], R[1], R[2], R[3], R[4], R[5]};
    struct win *w = rg_desktop && (int32_t)R[1] >= 0 ? icon_of(R[0], R[1]) : NULL;
    if (w && (w32(w->icon[R[1]] + 16) & 0x101) == 0x101) {
        uint32_t buf = w32(w->icon[R[1]] + 20), len = w32(w->icon[R[1]] + 28), n = 0;
        while (n < len && MEM[buf + n] >= 32) n++;
        if ((int32_t)c[5] < 0 || c[5] > n) c[5] = n;
        if ((int32_t)c[4] < 0) c[4] = 40;
    }
    wimp_set_caret(c);
}

/* ---------------------------------------------------------------- polling
 * Without the desktop, Wimp_Poll returns the events queued with
 * roger_wimp_event(), calling the wimp_idle hook when there are none.
 * On the desktop it returns, in order: redraw requests, queued events (the
 * user's, from desktop.c, and the program's own messages), and a null
 * event if it takes them (R0 bit 0 clear; Wimp_PollIdle: once the time in
 * R2 has come) - and until then lets time pass with the desktop shown.
 * Masked events are dropped, except that an Open_Window_Request the
 * program doesn't take is carried out by the Wimp itself. */
static int pop_event(uint32_t mask, uint32_t blk)
{
    for (int k = 0; k < WIMP_MAXWIN; k++) {
        struct win *w = &wins[k];
        if (w->handle && w->redraw && (w32(w->blk + 28) & WIN_OPEN)) {
            w->redraw = 0;
            WR32(blk, w->handle);
            R[0] = 1;
            return 1;
        }
    }
    while (qhead != qtail) {
        unsigned k = qhead++ % QLEN;
        uint32_t r = queue[k].reason;
        if (r < 32 && (mask >> r & 1)) {
            struct win *w = win_of(queue[k].block[0]);
            if (r == 2 && w && w != &iconbar) wimp_open(w, (uint8_t *)(queue[k].block + 1), queue[k].block[7]);
            continue;
        }
        memcpy(MEM + blk, queue[k].block, queue[k].n * 4);
        R[0] = r;
        filer_delivered(r, queue[k].block);
        return 1;
    }
    return 0;
}

static void poll(uint32_t b)              /* Wimp_Poll: R0 mask, R1 block (Wimp_PollIdle: R2 time) */
{
    filer_polled();
    if (rg_desktop) {
        uint32_t mask = R[0], blk = R[1], until = R[2];
        desktop_enter();
        for (;;) {
            desktop_input();
            if (pop_event(mask, blk)) break;
            if (!(mask & 1) && (b != 0x400E1 || (int32_t)(monotonic - until) >= 0)) { R[0] = 0; return; }
            desktop_wait();
        }
        ev("Wimp_Poll", "i", R[0]);
        return;
    }
    if (qhead == qtail && rg->wimp_idle) rg->wimp_idle();
    if (qhead == qtail) {
        if (R[0] & 1) finish("Wimp_Poll with nothing to do");
        R[0] = 0;                         /* Null_Reason_Code */
        return;
    }
    unsigned k = qhead++ % QLEN;
    memcpy(MEM + R[1], queue[k].block, queue[k].n * 4);
    R[0] = queue[k].reason;
    filer_delivered(R[0], queue[k].block);
    ev("Wimp_Poll", "i", R[0]);
}

/* ---------------------------------------------------------------- redrawing
 * A window the program draws itself, on the desktop: Wimp_RedrawWindow
 * (after a Redraw_Window_Request) and Wimp_UpdateWindow give it its
 * visible area as one rectangle, as the graphics window - cleared to its
 * background colour for a redraw, and holding what it drew before for an
 * update - and it draws there, in screen memory, with the VDU drivers;
 * Wimp_GetRectangle then keeps what it drew (struct win.back) for the
 * desktop to show. Without the desktop there's nothing to draw. */
static struct win *redrawing;

static void vdu_window(int32_t x0, int32_t y0, int32_t x1, int32_t y1)   /* VDU 24, inclusive */
{
    int32_t v[4] = {x0, y0, x1, y1};
    vdu_put(24);
    for (int k = 0; k < 4; k++) { vdu_put(v[k] & 0xFF); vdu_put(v[k] >> 8 & 0xFF); }
}

/* Where the program draws a window: where it is, or (on a desktop larger
 * than the program's screen) moved as little as will put it on the screen
 * - the redraw block gives it there, and the desktop shows it where it is */
static void canvas_shift(const struct win *w, int32_t *dx, int32_t *dy)
{
    const struct rg_mode *m = cur_mode();
    int32_t sw = m->width << m->xeig, sh = m->height << m->yeig;
    int32_t vx0 = (int32_t)w32(w->blk), vy0 = (int32_t)w32(w->blk + 4);
    int32_t vx1 = (int32_t)w32(w->blk + 8), vy1 = (int32_t)w32(w->blk + 12);
    *dx = vx1 > sw ? sw - vx1 : 0;
    if (vx0 + *dx < 0) *dx = -vx0;
    *dy = vy1 > sh ? sh - vy1 : 0;
    if (vy0 + *dy < 0) *dy = -vy0;
}

static void redraw_start(uint32_t blk, int update)
{
    struct win *w = win_of(RD32(blk));
    if (!rg_desktop || !w || w == &iconbar || !(w32(w->blk + 28) & WIN_OPEN)) { R[0] = 0; return; }
    int32_t dx, dy;
    canvas_shift(w, &dx, &dy);
    const struct rg_mode *m = cur_mode();
    int32_t sw = m->width << m->xeig, sh = m->height << m->yeig;
    int32_t vx0 = (int32_t)w32(w->blk) + dx, vy0 = (int32_t)w32(w->blk + 4) + dy;
    int32_t vx1 = (int32_t)w32(w->blk + 8) + dx, vy1 = (int32_t)w32(w->blk + 12) + dy;
    int32_t r[4] = {vx0, vy0, vx1, vy1};
    uint32_t size = screen_size(), base = screen_base();
    if (update) {                                    /* +4 a work area box */
        int32_t ox = vx0 - (int32_t)w32(w->blk + 16), oy = vy1 - (int32_t)w32(w->blk + 20);
        int32_t u[4] = {(int32_t)RD32(blk + 4) + ox, (int32_t)RD32(blk + 8) + oy,
                        (int32_t)RD32(blk + 12) + ox, (int32_t)RD32(blk + 16) + oy};
        if (u[0] > r[0]) r[0] = u[0];
        if (u[1] > r[1]) r[1] = u[1];
        if (u[2] < r[2]) r[2] = u[2];
        if (u[3] < r[3]) r[3] = u[3];
        if (w->back && w->back_size == size && !memcmp(w->back_vis, w->blk, 24))
            memcpy(MEM + base, w->back, size);       /* what it drew before */
    }
    if (r[0] < 0) r[0] = 0;
    if (r[1] < 0) r[1] = 0;
    if (r[2] > sw) r[2] = sw;
    if (r[3] > sh) r[3] = sh;
    int32_t v[4] = {vx0, vy0, vx1, vy1};
    memcpy(MEM + blk + 4, v, 16);
    memcpy(MEM + blk + 20, w->blk + 16, 8);
    if (r[0] >= r[2] || r[1] >= r[3]) { R[0] = 0; return; }
    for (int k = 0; k < 4; k++) WR32(blk + 28 + 4 * k, r[k]);
    vdu_window(r[0], r[1], r[2] - 1, r[3] - 1);
    if (!update && w->blk[35] != 0xFF) {             /* the work area's background colour */
        vdu_put(18); vdu_put(0); vdu_put(128 | (w->blk[35] & 15));
        vdu_put(16);
    }
    redrawing = w;
    w->back_dx = dx; w->back_dy = dy;
    R[0] = 1;
}

static void redraw_next(void)                       /* Wimp_GetRectangle: there's only the one */
{
    struct win *w = redrawing;
    R[0] = 0;
    if (!w) return;
    redrawing = NULL;
    uint32_t size = screen_size();
    if (w->back_size != size) { free(w->back); w->back = malloc(size); w->back_size = size; }
    if (w->back) memcpy(w->back, MEM + screen_base(), size);
    memcpy(w->back_vis, w->blk, 24);
    const struct rg_mode *m = cur_mode();
    vdu_window(0, 0, (m->width << m->xeig) - 1, (m->height << m->yeig) - 1);
}

/* ---------------------------------------------------------------- SWIs */

int wimp_swi(uint32_t b)
{
    switch (b) {
    case 0x400C1: create_window(); return 1;                /* Wimp_CreateWindow */
    case 0x400C2: {                                         /* Wimp_CreateIcon: R1 window, icon block */
        struct win *w = win_of(RD32(R[1]));
        int i = w ? add_icon(w, MEM + R[1] + 4) : -1;
        if (i < 0) { VF = 1; return 1; }
        if (w == &iconbar) {                                /* on the icon bar: -2 left, -1 right */
            ev("Wimp_CreateIcon", "ii", RD32(R[1]), i);
            wimp_bar_side[i] = RD32(R[1]) == 0xFFFFFFFE ? -2 : -1;
        }
        R[0] = i;
        return 1;
    }
    case 0x400C3: {                                         /* Wimp_DeleteWindow: R1 -> handle */
        struct win *w = win_of(RD32(R[1]));
        if (w && w != &iconbar) {
            ev("Wimp_DeleteWindow", "i", w->handle);
            wimp_delete(w);
        }
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
        if (w && w != &iconbar) wimp_close(w);
        return 1;
    }
    case 0x400C8: redraw_start(R[1], 0); return 1;         /* Wimp_RedrawWindow */
    case 0x400C9: redraw_start(R[1], 1); return 1;         /* Wimp_UpdateWindow */
    case 0x400CA: redraw_next(); return 1;                 /* Wimp_GetRectangle */
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
    case 0x400D2: set_caret(); return 1;                    /* Wimp_SetCaretPosition: R0-R5 */
    case 0x400D3:                                           /* Wimp_GetCaretPosition: R1 block */
        for (int k = 0; k < 6; k++) WR32(R[1] + 4 * k, caret[k]);
        return 1;
    case 0x400D1:                                           /* Wimp_ForceRedraw: R0 window (-1 the */
        if (rg_desktop)                                     /* screen), R1-R4 the area */
            for (int k = 0; k < WIMP_MAXWIN; k++)
                if (wins[k].handle && (R[0] == 0xFFFFFFFF || wins[k].handle == R[0]) &&
                    !(w32(wins[k].blk + 28) & 0x10)) wins[k].redraw = 1;
        return 1;
    case 0x400D0: if (rg_desktop) desktop_drag_box(R[1]); return 1;          /* Wimp_DragBox */
    case 0x400E8: if (rg_desktop) desktop_create_submenu(R[1], R[2], R[3]); return 1;   /* CreateSubMenu */
    case 0x400E6: case 0x400F0:                             /* Wimp_SetColour, Wimp_TextColour: R0 */
        if (rg_desktop) {                                   /* colour, bit 7 background (bits 4-6 */
            if (b == 0x400E6) { vdu_put(18); vdu_put(R[0] >> 4 & 7); }   /* the GCOL action) */
            else vdu_put(17);
            vdu_put((R[0] & 15) | (R[0] & 0x80));
        }
        return 1;
    case 0x400DC: case 0x400D8:                             /* ProcessKey, SetPointerShape */
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
        else if (R[2] == FILER_WINDOW || R[2] == FILER_TASK || filer_owns(R[2])) filer_message(R[1], R[2]);
        else if (R[2] == 0xFFFFFFFE && filer_bar_icon() >= 0 && R[3] == (uint32_t)filer_bar_icon())
            filer_message(R[1], FILER_BAR);                 /* (a save dropped on its icon bar icon) */
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
        if (rg_desktop) desktop_enter();
        return 1;
    case 0x400C7: case 0x400E1: poll(b); return 1;          /* Wimp_Poll, Wimp_PollIdle */
    case 0x400D4:                                            /* Wimp_CreateMenu */
    {
        ev("Wimp_CreateMenu", "iii", R[1], R[2], R[3]);
        if (rg_desktop) { desktop_create_menu(R[1], R[2], R[3]); return 1; }
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
        if (rg_desktop) {
            int x, y; int32_t w, i;
            uint32_t buttons = desktop_pointer(&x, &y, &w, &i);
            WR32(R[1], x); WR32(R[1] + 4, y); WR32(R[1] + 8, buttons);
            WR32(R[1] + 12, w); WR32(R[1] + 16, i);
            return 1;
        }
        WR32(R[1], 640); WR32(R[1] + 4, 512); WR32(R[1] + 8, 0);
        WR32(R[1] + 12, filer_armed() ? FILER_WINDOW : 0xFFFFFFFF);   /* over the Filer's window, */
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
