/* The Filer: directory viewers, and its end of the data transfer protocol.
 *
 * On the desktop (--desktop) a directory viewer is a window like the
 * program's - wimp.c keeps it, desktop.c draws it - but owned by the Filer,
 * a task of its own run here, in-process: the user's actions on it come
 * here (filer_event) rather than to the program's Wimp_Poll. Its contents
 * are one directory's entries (dir_entries: the program's saved copies as
 * well as the originals), as RISC OS 3's large icons: each a sprite with
 * its name under it, on a fixed grid - every cell as wide as the longest
 * name needs (ten characters at least), as many columns as the window is
 * wide, rows as many as that takes - laid out again whenever the window's
 * width changes. Sprites: "file_xxx" for a file of type xxx, "directory",
 * an application's own (its !Sprites22, or !Sprites, merged into the
 * Wimp's pool as the Filer does when it sees one), from the pool or else
 * drawn by the desktop.
 *
 * The user: Select click selects an icon (only it), Adjust toggles one, a
 * click on the background clears the selection; double-click opens one -
 * a directory in a viewer of its own (Adjust closing this one), an
 * application run, a file offered to the program (Message_DataOpen; if
 * it doesn't take it, the file is run) - and dragging one out to the
 * program's window or icon bar icon gives it the file (Message_DataLoad).
 * Running is the game's (roger_config.filer_run): there is only the one
 * program. A viewer reads its directory again once a second, and when a
 * save to it is done.
 *
 * Saving: the program's save box dragged to a viewer sends
 * Message_DataSave to its window, answered with DataSaveAck naming the
 * file in that directory; the program saves it and says so with
 * Message_DataLoad, answered with DataLoadAck - and the viewer shows it.
 * Without the desktop there are no viewers, but game code can still stand
 * in for the user dragging a save to one: roger_wimp_save_to() arms a
 * destination, a viewer the program finds under the pointer
 * (FILER_WINDOW) until the save is done. */
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define NVIEW     8                       /* viewers open at once */
#define MARGIN    8                       /* around the grid (OS units) */
#define CELL_H    116                     /* a grid cell: a 68-unit sprite, its name under it */
#define CELL_MINW 176                     /* ...wide enough for ten characters at least */
#define VIEW_DATA 0x4000                  /* each viewer's indirected data in FILER_DATA: */
#define ICON_DATA 80                      /* its title (256 bytes), then per icon its name
                                             (64) and validation string (16) */

static struct viewer {
    struct win *w;                        /* NULL: not in use */
    char dir[256];                        /* the directory, as the program names files */
    struct dir_entry e[WIMP_MAXICON];
    uint8_t sel[WIMP_MAXICON];
    int n, cols, cell_w;
} views[NVIEW];

static struct viewer *view_of(uint32_t handle)
{
    for (int k = 0; k < NVIEW; k++) if (views[k].w && views[k].w->handle == handle) return &views[k];
    return NULL;
}
int filer_owns(uint32_t handle) { return view_of(handle) != NULL; }

static uint32_t view_data(const struct viewer *v) { return FILER_DATA + (uint32_t)(v - views) * VIEW_DATA; }
static int is_app(const struct dir_entry *e) { return e->isdir && e->leaf[0] == '!'; }
static uint32_t entry_type(const struct dir_entry *e)   /* &1000 directory, &2000 application */
{
    if (e->isdir) return is_app(e) ? 0x2000 : 0x1000;
    return (e->load >> 20) == 0xFFF ? (e->load >> 8) & 0xFFF : 0xFFD;
}
static void entry_name(const struct viewer *v, int i, char *out, int size)
{
    snprintf(out, size, "%.255s.%.63s", v->dir, v->e[i].leaf);
}

/* ---------------------------------------------------------------- application sprites
 * An application's !Sprites22 (square pixels, as the desktop's are) or
 * !Sprites, merged into the Wimp's pool the first time it's seen */
static char merged[32][256];
static int nmerged;

void filer_app_sprites(const char *hostdir)
{
    for (int k = 0; k < nmerged; k++) if (!strcmp(merged[k], hostdir)) return;
    if (nmerged < 32) snprintf(merged[nmerged++], sizeof merged[0], "%s", hostdir);
    char path[2048];
    DIR *d = opendir(hostdir);
    struct dirent *e;
    int best = 0;
    while (d && (e = readdir(d))) {               /* (a host name may add ",type" or ",load-exec") */
        size_t l = strcspn(e->d_name, ",");
        int which = l == 10 && !strncasecmp(e->d_name, "!Sprites22", l) ? 2 :
                    l == 8 && !strncasecmp(e->d_name, "!Sprites", l) ? 1 : 0;
        if (which > best) { best = which; snprintf(path, sizeof path, "%s/%s", hostdir, e->d_name); }
    }
    if (d) closedir(d);
    if (best) sprite_file_host(wimp_sprites(), path, 1);
}

/* ---------------------------------------------------------------- layout */
static int extent_w(void) { int w, h; desktop_size(&w, &h); return w; }

/* The icons on the grid for a visible area width wd; the work area's
 * extent: as wide as the screen (a viewer can be widened to show more
 * columns), as deep as the rows */
static void layout(struct viewer *v, int wd)
{
    struct win *w = v->w;
    v->cols = (wd - 2 * MARGIN) / v->cell_w;
    if (v->cols < 1) v->cols = 1;
    int rows = (v->n + v->cols - 1) / v->cols;
    if (rows < 1) rows = 1;
    uint32_t d = view_data(v) + 256;
    for (int i = 0; i < v->n; i++, d += ICON_DATA) {
        const struct dir_entry *e = &v->e[i];
        char sprite[16];
        if (is_app(e)) {
            snprintf(sprite, sizeof sprite, "%.12s", e->leaf);
            for (char *c = sprite; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        } else if (e->isdir) strcpy(sprite, "directory");
        else if ((e->load >> 20) == 0xFFF) snprintf(sprite, sizeof sprite, "file_%03x", (e->load >> 8) & 0xFFF);
        else strcpy(sprite, "file_xxx");
        size_t l = strlen(e->leaf);
        memcpy(MEM + d, e->leaf, l + 1);
        MEM[d + 64] = 'S';
        memcpy(MEM + d + 65, sprite, strlen(sprite) + 1);
        int col = i % v->cols, row = i / v->cols;
        int32_t x0 = MARGIN + col * v->cell_w, top = -MARGIN - row * CELL_H;
        uint32_t ic[8] = {(uint32_t)x0, (uint32_t)(top - CELL_H + 8), (uint32_t)(x0 + v->cell_w), (uint32_t)(top - 4),
                          0x1700A10Bu | (v->sel[i] ? 1u << 21 : 0),   /* text and sprite, centred, */
                          d, d + 64, (uint32_t)l + 1};                /* indirected; click/drag/double */
        memcpy(w->icon[i], ic, 32);
        w->used[i] = 1;
    }
    for (int i = v->n; i < w->nicons; i++) w->used[i] = 0;
    w->nicons = v->n;
    int32_t ext[4] = {0, -(2 * MARGIN + rows * CELL_H), extent_w(), 0};
    memcpy(w->blk + 40, ext, 16);
}

/* (re)read the directory; 0 if it's gone */
static int scan(struct viewer *v)
{
    static struct dir_entry e[WIMP_MAXICON];
    int n = dir_entries(v->dir, e, WIMP_MAXICON);
    if (n < 0) return 0;
    uint8_t sel[WIMP_MAXICON] = {0};
    int longest = 10;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < v->n; j++) if (v->sel[j] && !strcmp(v->e[j].leaf, e[i].leaf)) sel[i] = 1;
        int l = (int)strlen(e[i].leaf);
        if (l > longest) longest = l;
        if (is_app(&e[i])) {
            char name[400], host[2048];
            snprintf(name, sizeof name, "%.255s.%.63s", v->dir, e[i].leaf);
            if (hostpath_read(name, host)) filer_app_sprites(host);
        }
    }
    memcpy(v->e, e, n * sizeof *e);
    memcpy(v->sel, sel, sizeof sel);
    v->n = n;
    v->cell_w = longest * 16 + 16 > CELL_MINW ? longest * 16 + 16 : CELL_MINW;   /* (16 a character) */
    return 1;
}

/* open (or re-open) at a visible area: laid out for its width, no deeper
 * than its rows, scrolled within them */
static void open_at(struct viewer *v, const int32_t *vis, int32_t sy, uint32_t behind)
{
    layout(v, vis[2] - vis[0]);
    int32_t ey0 = (int32_t)wimp_rd(v->w->blk + 44);
    int32_t o[6] = {vis[0], vis[1], vis[2], vis[3], 0, sy};
    if (o[3] - o[1] > -ey0) o[1] = o[3] + ey0;
    if (o[5] < ey0 + (o[3] - o[1])) o[5] = ey0 + (o[3] - o[1]);
    if (o[5] > 0) o[5] = 0;
    wimp_open(v->w, (const uint8_t *)o, behind);
}

static void refresh(struct viewer *v)
{
    char before[sizeof v->e];
    int n = v->n;
    memcpy(before, v->e, sizeof before);
    if (!scan(v)) { wimp_delete(v->w); v->w = NULL; return; }
    if (n == v->n && !memcmp(before, v->e, n * sizeof *v->e)) return;
    int32_t vis[4];
    memcpy(vis, v->w->blk, 16);
    open_at(v, vis, (int32_t)wimp_rd(v->w->blk + 20), 0xFFFFFFFD);   /* (-3: where it is in the stack) */
}

void filer_refresh(void)
{
    for (int k = 0; k < NVIEW; k++) if (views[k].w) refresh(&views[k]);
}

/* once a second on the desktop: files the program wrote itself appear,
 * and deleted ones go */
void filer_tick(void)
{
    static uint32_t last;
    if (monotonic - last < 100) return;
    last = monotonic;
    filer_refresh();
}

/* ---------------------------------------------------------------- opening a viewer */
int filer_open(const char *dir)
{
    char name[1100];
    gstrans(dir, name, sizeof name);
    size_t l = strlen(name);
    while (l && (name[l - 1] == ' ' || name[l - 1] == '.')) name[--l] = 0;
    if (!rg_desktop || !*name) return 0;
    for (int k = 0; k < NVIEW; k++)               /* open already: to the front */
        if (views[k].w && !strcasecmp(views[k].dir, name)) {
            uint8_t o[24];
            memcpy(o, views[k].w->blk, 24);
            wimp_open(views[k].w, o, 0xFFFFFFFF);
            return 1;
        }
    struct viewer *v = NULL;
    int k;
    for (k = 0; k < NVIEW && !v; k++) if (!views[k].w) v = &views[k];
    if (!v) return 0;
    k--;
    memset(v, 0, sizeof *v);
    snprintf(v->dir, sizeof v->dir, "%s", name);
    if (!scan(v)) { fprintf(stderr, "Filer: no directory %s\n", name); return 0; }
    uint32_t title = view_data(v);
    snprintf((char *)MEM + title, 256, "%s", name);
    uint8_t blk[88] = {0};
    uint32_t words[] = {
        0, 0, 0, 0, 0, 0, 0xFFFFFFFF,
        0xBF000012u,                              /* back, close, title, toggle size, vertical scroll */
                                                  /* bar, adjust size; moveable, auto-redraw */
        0x01070207u,                              /* title 7 on 2, work area 7 on 1 */
        0x000C0103u,                              /* scroll bar 3 and 1, input focus 12 */
        0, 0, 0, 0,
        0x00000119u,                              /* title: indirected text, centred */
        0x0000A000u,                              /* work area: click, drag, double click */
        1,                                        /* sprites from the Wimp's pool */
        (uint32_t)(CELL_MINW + 2 * MARGIN) | (uint32_t)(CELL_H + 2 * MARGIN) << 16,
        title, 0xFFFFFFFF, 256, 0};
    memcpy(blk, words, sizeof words);
    v->w = wimp_new_window(blk);
    if (!v->w) return 0;
    v->w->filer = 1;
    int sw, sh;
    desktop_size(&sw, &sh);
    int cols = 4, rows = (v->n + cols - 1) / cols;   /* a standard size, not the contents' */
    if (rows < 1) rows = 1;
    if (rows > 3) rows = 3;
    int32_t wd = 2 * MARGIN + cols * v->cell_w, ht = 2 * MARGIN + rows * CELL_H;
    int32_t x0 = 192 + 64 * k, y1 = sh - 160 - 64 * k;
    if (x0 + wd > sw - 64) x0 = sw - 64 - wd;
    if (x0 < 64) x0 = 64;
    int32_t vis[4] = {x0, y1 - ht, x0 + wd, y1};
    open_at(v, vis, 0, 0xFFFFFFFF);
    return 1;
}

int roger_filer_open(const char *dir) { return filer_open(dir); }

void filer_close(const char *dir)                /* *Filer_CloseDir */
{
    char name[1100];
    gstrans(dir, name, sizeof name);
    size_t l = strlen(name);
    while (l && (name[l - 1] == ' ' || name[l - 1] == '.')) name[--l] = 0;
    for (int k = 0; k < NVIEW; k++)
        if (views[k].w && !strncasecmp(views[k].dir, name, l) && (!views[k].dir[l] || views[k].dir[l] == '.')) {
            wimp_delete(views[k].w);
            views[k].w = NULL;
        }
}

/* ---------------------------------------------------------------- messages
 * The Filer's to the program: user messages (17), or recorded (18) - one
 * at a time - for the program to acknowledge; one it doesn't (it has
 * polled again without replying) has bounced */
static struct {
    uint32_t ref;                                 /* 0: none */
    int delivered;
    uint32_t action;
    char name[400];
} pending;

static uint32_t send(uint32_t reason, uint32_t action, uint32_t your_ref, const uint32_t *w6, const char *name)
{
    uint32_t b[64] = {0};
    size_t l = strlen(name);
    if (l > 200) l = 200;
    b[0] = (uint32_t)((44 + l + 1 + 3) & ~3u);
    b[1] = FILER_TASK; b[2] = wimp_new_ref(); b[3] = your_ref; b[4] = action;
    memcpy(b + 5, w6, 24);
    memcpy((char *)b + 44, name, l);
    roger_wimp_event(reason, b, (int)(b[0] / 4));
    if (reason == 18) {
        pending.ref = b[2]; pending.delivered = 0; pending.action = action;
        snprintf(pending.name, sizeof pending.name, "%s", name);
    }
    return b[2];
}

static void run(const char *name)                 /* what *Run would do: the game's to say */
{
    ev("Filer_Run", "s", name, (int)strlen(name));
    if (rg->filer_run) rg->filer_run(name);
    else fprintf(stderr, "Filer: nothing runs %s\n", name);
}

void filer_delivered(uint32_t reason, const uint32_t *blk)
{
    if (reason == 18 && pending.ref && blk[2] == pending.ref) pending.delivered = 1;
}

void filer_polled(void)
{
    if (!pending.ref || !pending.delivered) return;
    pending.ref = 0;
    if (pending.action == 5) run(pending.name);  /* Message_DataOpen not taken: run it */
}

/* Save to a viewer's directory (or, without the desktop, where game code
 * said: roger_wimp_save_to) */
static char save_to[1024];
void roger_wimp_save_to(const char *path) { snprintf(save_to, sizeof save_to, "%s", path ? path : ""); }
int filer_armed(void) { return save_to[0] != 0; }

/* A message from the program to the Filer: to one of its windows (to), or
 * the task (FILER_TASK) */
void filer_message(uint32_t m, uint32_t to)
{
    uint32_t action = RD32(m + 16), w6[6];
    memcpy(w6, MEM + m + 20, 24);
    char name[1100];
    if (action == 1) {                                       /* Message_DataSave: +44 a leaf name */
        struct viewer *v = view_of(to);
        if (v) {
            char leaf[256];
            cstr(m + 44, leaf, 1);
            const char *dot = strrchr(leaf, '.');
            snprintf(name, sizeof name, "%s.%s", v->dir, dot ? dot + 1 : leaf);
        } else if (save_to[0] && (to == FILER_WINDOW || to == FILER_TASK)) snprintf(name, sizeof name, "%s", save_to);
        else return;
        send(17, 2, RD32(m + 8), w6, name);                 /* DataSaveAck: the full name */
    } else if (action == 3) {                                /* Message_DataLoad: saved */
        cstr(m + 44, name, 0);
        ev("Filer_saved", "s", name, (int)strlen(name));
        send(17, 4, RD32(m + 8), w6, name);                 /* DataLoadAck */
        save_to[0] = 0;
        if (rg_desktop) filer_refresh();
    } else if (action == 4) {                                /* DataLoadAck: it took ours */
        if (pending.ref && RD32(m + 12) == pending.ref) pending.ref = 0;
    }
}

/* ---------------------------------------------------------------- the user */
static struct viewer *drag_view;
static int drag_icon;

static void select_only(struct viewer *v, int icon)
{
    memset(v->sel, 0, sizeof v->sel);
    if (icon >= 0 && icon < v->n) v->sel[icon] = 1;
}

static void open_entry(struct viewer *v, int i, int adjust)
{
    char name[400];
    entry_name(v, i, name, sizeof name);
    v->sel[i] = 0;
    const struct dir_entry *e = &v->e[i];
    if (e->isdir && !is_app(e)) {
        uint32_t h = v->w->handle;
        filer_open(name);
        if (adjust && (v = view_of(h))) { wimp_delete(v->w); v->w = NULL; }   /* Adjust: instead of this one */
        return;
    }
    if (is_app(e)) { run(name); return; }
    uint32_t w6[6] = {v->w->handle, 0xFFFFFFFF, (uint32_t)mouse_x, (uint32_t)mouse_y, 0, entry_type(e)};
    send(18, 5, 0, w6, name);                               /* Message_DataOpen */
}

/* desktop.c's events for the Filer's windows */
void filer_event(uint32_t reason, const uint32_t *blk)
{
    struct viewer *v = view_of(reason == 6 ? blk[3] : blk[0]);
    if (!v) return;
    switch (reason) {
    case 2: {                                                /* Open_Window_Request */
        int32_t vis[4];
        memcpy(vis, blk + 1, 16);
        open_at(v, vis, (int32_t)blk[6], blk[7]);
        break;
    }
    case 3:                                                  /* Close_Window_Request */
        wimp_delete(v->w);
        v->w = NULL;
        break;
    case 6: {                                                /* Mouse_Click */
        uint32_t b = blk[2];
        int icon = (int32_t)blk[4] >= 0 && (int32_t)blk[4] < v->n ? (int)blk[4] : -1;
        if (b == 0x400) select_only(v, icon);                /* Select */
        else if (b == 0x100 && icon >= 0) v->sel[icon] ^= 1; /* Adjust */
        else if ((b == 4 || b == 1) && icon >= 0) open_entry(v, icon, b == 1);
        else if ((b == 0x40 || b == 0x10) && icon >= 0) {    /* a drag: the file goes where it's dropped */
            if (!v->sel[icon]) { if (b == 0x40) select_only(v, icon); else v->sel[icon] = 1; }
            const uint8_t *b2 = v->w->blk;
            int32_t ox = (int32_t)wimp_rd(b2) - (int32_t)wimp_rd(b2 + 16), oy = (int32_t)wimp_rd(b2 + 12) - (int32_t)wimp_rd(b2 + 20);
            const uint8_t *ic = v->w->icon[icon];
            int32_t box[4] = {ox + (int32_t)wimp_rd(ic), oy + (int32_t)wimp_rd(ic + 4),
                              ox + (int32_t)wimp_rd(ic + 8), oy + (int32_t)wimp_rd(ic + 12)};
            drag_view = v; drag_icon = icon;
            desktop_drag_filer(box);
        }
        if (v->w) layout(v, (int32_t)wimp_rd(v->w->blk + 8) - (int32_t)wimp_rd(v->w->blk));   /* (the selection) */
        break;
    }
    }
}

/* Game code (filer_run): a file or application given to the program as
 * if dragged from a viewer to a window and icon of its (-2: the icon bar) */
int roger_filer_load(const char *name, int32_t window, int32_t icon)
{
    char host[2048];
    if (!hostpath_read(name, host)) return 0;
    struct stat st;
    if (stat(host, &st)) return 0;
    const char *leaf = strrchr(name, '.');
    struct dir_entry e = {.isdir = S_ISDIR(st.st_mode), .size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size};
    snprintf(e.leaf, sizeof e.leaf, "%.63s", leaf ? leaf + 1 : name);
    uint32_t exec;
    file_info(host, &e.load, &exec);
    uint32_t w6[6] = {(uint32_t)window, (uint32_t)icon, (uint32_t)mouse_x, (uint32_t)mouse_y, e.size, entry_type(&e)};
    send(18, 3, 0, w6, name);
    return 1;
}

/* the end of a drag from a viewer: to the program, Message_DataLoad */
void filer_drag_end(void)
{
    struct viewer *v = drag_view;
    drag_view = NULL;
    if (!v || !v->w || drag_icon >= v->n) return;
    int x, y;
    int32_t window, icon;
    desktop_pointer(&x, &y, &window, &icon);
    if (window == -1 || filer_owns((uint32_t)window)) return;   /* nowhere, or a viewer: no copying */
    char name[400];
    entry_name(v, drag_icon, name, sizeof name);
    const struct dir_entry *e = &v->e[drag_icon];
    uint32_t w6[6] = {(uint32_t)window, (uint32_t)icon, (uint32_t)x, (uint32_t)y, e->size, entry_type(e)};
    send(18, 3, 0, w6, name);
}
