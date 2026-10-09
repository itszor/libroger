/* The desktop (--desktop): the Window Manager's screen drawn and used for
 * real, for a program that is a Wimp task - in place of the wimp_idle
 * hook, the user works its icon bar icon, menus and windows.
 *
 * A screen the size of the window, pixel for pixel (each 2x2 OS units, so
 * a 640x512 window is the 1280x1024 OS units a program expects; a larger
 * one, more desktop - never scaled), in RISC OS 3.1's look: the backdrop
 * (desktop/backdrop.png, embedded), the icon bar along the bottom - the
 * program's icons on its right (window -1) or left (-2) - the program's
 * windows with their furniture (title bar, back, close and toggle-size
 * icons, scroll bars, adjust-size icon) and icons, menus and dialogue boxes
 * opened as menus, the caret, and a drag box. All of it is drawn here, in
 * host colours, from the windows and icons wimp.c keeps as the program
 * defines them. What a program draws in a window itself (one not
 * auto-redrawn) it draws in screen memory, with the VDU drivers, when
 * asked to redraw (wimp.c); that picture is shown in its window.
 *
 * The desktop shows from Wimp_Initialise and whenever the program is in
 * Wimp_Poll; a mode change after that (the program taking the screen for
 * itself) shows the program's screen instead - fullscreen, unless
 * --windowed-games - until it polls again.
 * Input is taken only while the program is in Wimp_Poll, as the Wimp
 * does: mouse_x/mouse_y/mouse_buttons and the keyboard, so --script drives
 * it too, in headless runs as well (screenshots show the desktop). */
#include <SDL3/SDL.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

int rg_desktop;
static int visible;
int desktop_visible(void) { return visible; }

static int dw, dh;                                /* the desktop's size in pixels: the window's */
#define SW (2 * dw)                               /* ...in OS units */
#define SH (2 * dh)
#define BAR_H      96                     /* the icon bar's height (OS units) */
#define BAR_ORG    16                     /* its icons' y origin */
#define BAR_GAP    24                     /* between its icons, and at the ends */
#define B          2                      /* furniture: a border line */
#define T          40                     /* ...the title bar's height, a scroll bar's width */
#define TB         (T + 2 * B)            /* ...a furniture icon, with its borders */
#define MENU_TICK  24                     /* menus: room for a tick, left of the text */
#define MENU_ARROW 32                     /* ...and for a submenu arrow at the right */
#define MENU_SEP   24                     /* ...a dotted line between items */
#define CHAR_W     16                     /* the system font: 16x32 OS units */
#define CHAR_H     32

static uint32_t *fb;

/* The Wimp's 16 colours */
static const uint32_t wimp_rgb[16] = {
    0xFFFFFF, 0xDDDDDD, 0xBBBBBB, 0x999999, 0x777777, 0x555555, 0x333333, 0x000000,
    0x004499, 0xEEEE00, 0x00CC00, 0xDD0000, 0xEEEEBB, 0x558800, 0xFFBB00, 0x00BBFF};
static const uint16_t wimp_vidc[16] = {   /* the same, for VIDC (&BGR) */
    0xFFF, 0xDDD, 0xBBB, 0x999, 0x777, 0x555, 0x333, 0x000,
    0x940, 0x0EE, 0x0C0, 0x00D, 0xBEE, 0x085, 0x0BF, 0xFB0};
#define COL(n) wimp_rgb[(n) & 15]

static uint32_t rd(uint32_t a) { return a + 4 <= MEMSIZE ? RD32(a) : 0; }
static int32_t  rdi(const uint8_t *p) { return (int32_t)wimp_rd(p); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ---------------------------------------------------------------- drawing
 * Pixels (x right, y down) and OS units (x right, y up: a pixel covers 2x2) */
#define PX(x) ((x) >> 1)
#define PY(y) ((SH - (y)) >> 1)
static int cl_x0, cl_y0, cl_x1, cl_y1;

static void clip_px(int x0, int y0, int x1, int y1)
{
    cl_x0 = imax(x0, 0); cl_y0 = imax(y0, 0); cl_x1 = imin(x1, dw); cl_y1 = imin(y1, dh);
}
static void clip_os(int x0, int y0, int x1, int y1) { clip_px(PX(x0), PY(y1), PX(x1), PY(y0)); }
static void clip_and_os(int x0, int y0, int x1, int y1)   /* narrow the clip */
{
    cl_x0 = imax(cl_x0, PX(x0)); cl_y0 = imax(cl_y0, PY(y1));
    cl_x1 = imin(cl_x1, PX(x1)); cl_y1 = imin(cl_y1, PY(y0));
}
static void clip_all(void) { clip_px(0, 0, dw, dh); }
static void clip_save(int *c) { c[0] = cl_x0; c[1] = cl_y0; c[2] = cl_x1; c[3] = cl_y1; }
static void clip_restore(const int *c) { cl_x0 = c[0]; cl_y0 = c[1]; cl_x1 = c[2]; cl_y1 = c[3]; }

static void dot(int x, int y, uint32_t c)
{
    if (x >= cl_x0 && x < cl_x1 && y >= cl_y0 && y < cl_y1) fb[y * dw + x] = c;
}
static void fill_px(int x0, int y0, int x1, int y1, uint32_t c)
{
    x0 = imax(x0, cl_x0); y0 = imax(y0, cl_y0); x1 = imin(x1, cl_x1); y1 = imin(y1, cl_y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) fb[y * dw + x] = c;
}
static void frame_px(int x0, int y0, int x1, int y1, uint32_t c)   /* 1 pixel inside the rectangle */
{
    fill_px(x0, y0, x1, y0 + 1, c); fill_px(x0, y1 - 1, x1, y1, c);
    fill_px(x0, y0, x0 + 1, y1, c); fill_px(x1 - 1, y0, x1, y1, c);
}
static void fill_os(int x0, int y0, int x1, int y1, uint32_t c) { fill_px(PX(x0), PY(y1), PX(x1), PY(y0), c); }
static void frame_os(int x0, int y0, int x1, int y1, uint32_t c) { frame_px(PX(x0), PY(y1), PX(x1), PY(y0), c); }
static void dotted_px(int x0, int y0, int x1, int y1, uint32_t c, int phase)   /* a dotted outline */
{
    for (int x = x0; x < x1; x++) if (((x + phase) >> 1) & 1) { dot(x, y0, c); dot(x, y1 - 1, c); }
    for (int y = y0; y < y1; y++) if (((y + phase) >> 1) & 1) { dot(x0, y, c); dot(x1 - 1, y, c); }
}
static uint32_t blend(uint32_t a, uint32_t b)              /* halfway */
{
    return ((a >> 1) & 0x7F7F7F) + ((b >> 1) & 0x7F7F7F);
}

/* Text in the system font: the 8x8 character set (font.c, Acorn Mode 0)
 * drawn double height, 8x16 pixels (16x32 OS units). x, y the top left in
 * pixels. */
extern const uint8_t roger_font[256][8];
static void text_px(int x, int y, const char *s, int n, uint32_t c)
{
    for (int k = 0; k < n; k++, x += 8)
        for (int r = 0; r < 8; r++) {
            uint8_t bits = roger_font[(uint8_t)s[k]][r];
            for (int b = 0; bits && b < 8; b++)
                if (bits & (0x80 >> b)) { dot(x + b, y + 2 * r, c); dot(x + b, y + 2 * r + 1, c); }
        }
}
static void text_os(int x, int ytop, const char *s, int n, uint32_t c) { text_px(PX(x), PY(ytop), s, n, c); }

/* ---------------------------------------------------------------- the backdrop
 * desktop/backdrop.png, embedded (src/backdrop.S), scaled to cover the
 * screen above the icon bar - cropped more at the left than the right -
 * whenever the desktop's size changes */
extern const uint8_t roger_backdrop_png[], roger_backdrop_png_end[];
static uint32_t *picture, *backdrop;              /* as loaded; as shown (dw x the height above the bar) */
static int pic_w, pic_h;

void desktop_init(void)
{
    SDL_IOStream *io = SDL_IOFromConstMem(roger_backdrop_png, (size_t)(roger_backdrop_png_end - roger_backdrop_png));
    SDL_Surface *s = io ? SDL_LoadPNG_IO(io, true) : NULL;
    SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_XRGB8888) : NULL;
    if (c && (picture = SDL_malloc((size_t)c->w * c->h * 4))) {
        pic_w = c->w; pic_h = c->h;
        for (int y = 0; y < pic_h; y++) memcpy(picture + (size_t)y * pic_w, (uint8_t *)c->pixels + y * c->pitch, (size_t)pic_w * 4);
    } else fprintf(stderr, "desktop: no backdrop (%s)\n", SDL_GetError());
    SDL_DestroySurface(c);
    SDL_DestroySurface(s);
}

/* the picture scaled by area (each pixel the average of what it covers) */
static void scale_backdrop(int w, int h)
{
    SDL_free(backdrop);
    backdrop = NULL;
    if (!picture || w <= 0 || h <= 0 || !(backdrop = SDL_malloc((size_t)w * h * 4))) return;
    double sc = (double)w / pic_w > (double)h / pic_h ? (double)w / pic_w : (double)h / pic_h;
    double ox = (pic_w * sc - w) * 0.8, oy = (pic_h * sc - h) / 2;   /* the crop */
    for (int y = 0; y < h; y++) {
        double sy0 = (y + oy) / sc, sy1 = (y + 1 + oy) / sc;
        for (int x = 0; x < w; x++) {
            double sx0 = (x + ox) / sc, sx1 = (x + 1 + ox) / sc, acc[3] = {0, 0, 0}, area = 0;
            for (int py = (int)sy0; py < sy1 && py < pic_h; py++) {
                double fy = (py + 1 < sy1 ? py + 1 : sy1) - (py > sy0 ? py : sy0);
                for (int px = (int)sx0; px < sx1 && px < pic_w; px++) {
                    double f = fy * ((px + 1 < sx1 ? px + 1 : sx1) - (px > sx0 ? px : sx0));
                    uint32_t c = picture[(size_t)py * pic_w + px];
                    acc[0] += f * (c >> 16 & 0xFF); acc[1] += f * (c >> 8 & 0xFF); acc[2] += f * (c & 0xFF);
                    area += f;
                }
            }
            if (area <= 0) area = 1;
            backdrop[(size_t)y * w + x] = (uint32_t)(acc[0] / area + 0.5) << 16 | (uint32_t)(acc[1] / area + 0.5) << 8 |
                                          (uint32_t)(acc[2] / area + 0.5);
        }
    }
}

/* The desktop's size: the window's in pixels (interactive), or as
 * --desktop=WxH gave it */
int rg_desktop_w = 1024, rg_desktop_h = 768;
static void fit_window(void)
{
    int w = rg_desktop_w, h = rg_desktop_h;
    if (rg_interactive) display_output_size(&w, &h);
    w = imax(320, imin(w, 4096)); h = imax(240, imin(h, 4096));
    if (w == dw && h == dh && fb) return;
    uint32_t *n = SDL_realloc(fb, (size_t)w * h * 4);
    if (!n) return;
    fb = n; dw = w; dh = h;
    scale_backdrop(dw, PY(BAR_H));
}

/* ---------------------------------------------------------------- sprites
 * A sprite: +0 next, +4 name, +16 width in words - 1, +20 height - 1, +24
 * first bit used, +28 last bit used (of each row's last word), +32 image,
 * +36 mask (= image: none), +40 mode, then perhaps a palette (pairs of
 * &BBGGRR00 words). An old mode number gives the pixel format and shape; a
 * RISC OS 3.5 sprite type word its depth and dpi (and a 1bpp mask). */
struct spr {
    uint32_t s, img, mask, pal;
    int w, h, wwords, bpp, xeig, yeig, first, newmask, npal;
    int builtin;                          /* or one of the Wimp's own, drawn here (1...) */
    int wos, hos;                         /* size in OS units */
};

static int sprite_info(uint32_t s, struct spr *p)
{
    static const uint8_t mode_fmt[29][3] = {   /* bpp, xeig, yeig */
        {1,1,2},{2,2,2},{4,3,2},{2,1,2},{1,2,2},{2,3,2},{1,2,2},{4,2,2},{2,1,2},{4,2,2},{8,3,2},
        {2,1,2},{4,1,2},{8,2,2},{4,1,2},{8,1,2},{4,1,2},{4,1,2},{1,1,1},{2,1,1},{4,1,1},{8,1,1},
        {4,0,1},{1,1,1},{8,1,2},{1,1,1},{2,1,1},{4,1,1},{8,1,1}};
    memset(p, 0, sizeof *p);
    if (!s || s + 44 > MEMSIZE) return 0;
    uint32_t mode = rd(s + 40);
    p->s = s;
    if (mode < 256) {
        if ((mode & 0x7F) > 28) return 0;
        p->bpp = mode_fmt[mode & 0x7F][0]; p->xeig = mode_fmt[mode & 0x7F][1]; p->yeig = mode_fmt[mode & 0x7F][2];
    } else {
        static const uint8_t type_bpp[8] = {0, 1, 2, 4, 8, 16, 32, 0};
        int xdpi = mode >> 1 & 0x1FFF, ydpi = mode >> 14 & 0x1FFF;
        p->bpp = type_bpp[mode >> 27 & 7];
        if (!p->bpp) return 0;
        p->xeig = xdpi >= 180 ? 0 : xdpi >= 90 ? 1 : 2;
        p->yeig = ydpi >= 180 ? 0 : ydpi >= 90 ? 1 : 2;
        p->newmask = 1;
    }
    p->wwords = (int)rd(s + 16) + 1;
    p->h = (int)rd(s + 20) + 1;
    p->first = p->newmask ? 0 : (int)(rd(s + 24) & 31);
    int last = (int)(rd(s + 28) & 31);
    p->w = (p->wwords * 32 - p->first - (31 - last)) / p->bpp;
    p->img = s + rd(s + 32);
    p->mask = s + rd(s + 36);
    if (p->mask == p->img) p->mask = 0;
    if (rd(s + 32) > 44) { p->pal = s + 44; p->npal = (int)(rd(s + 32) - 44) / 8; }
    if (p->w <= 0 || p->h <= 0 || p->wwords > 1024 || p->h > 1024 || p->img + (uint32_t)p->wwords * 4 * p->h > MEMSIZE) return 0;
    p->wos = p->w << p->xeig; p->hos = p->h << p->yeig;
    return 1;
}

static uint32_t find_sprite(uint32_t area, const char *name)
{
    if (area == 1) area = wimp_sprites();
    if (!area || area + 16 > MEMSIZE) return 0;
    uint32_t n = rd(area + 4), s = area + rd(area + 8);
    for (uint32_t k = 0; k < n && k < 4096 && s + 16 < MEMSIZE; k++) {
        char nm[13] = {0};
        memcpy(nm, MEM + s + 4, 12);
        for (int i = 0; i < 12; i++) if ((uint8_t)nm[i] <= 32) nm[i] = 0;
        if (!strcasecmp(nm, name)) return s;
        uint32_t next = rd(s);
        if (!next) break;
        s += next;
    }
    return 0;
}

/* The Wimp's own sprites (its ROM pool) a program may name, drawn here */
enum { BI_NONE, BI_OPTOFF, BI_OPTON, BI_RADIOOFF, BI_RADIOON, BI_DIR, BI_APP, BI_FILE };
static int builtin_of(const char *name)
{
    if (!strcasecmp(name, "optoff")) return BI_OPTOFF;
    if (!strcasecmp(name, "opton")) return BI_OPTON;
    if (!strcasecmp(name, "radiooff")) return BI_RADIOOFF;
    if (!strcasecmp(name, "radioon")) return BI_RADIOON;
    if (!strcasecmp(name, "directory") || !strcasecmp(name, "small_dir")) return BI_DIR;
    if (name[0] == '!' || !strcasecmp(name, "application") || !strcasecmp(name, "small_app")) return BI_APP;
    if (!strncasecmp(name, "file_", 5) || !strncasecmp(name, "small_", 6)) return BI_FILE;
    return BI_NONE;
}

/* a sprite by name: the window's area, then the Wimp's pool, then the
 * Wimp's own; 0 if none */
static int lookup_sprite(uint32_t area, const char *name, struct spr *p)
{
    uint32_t s = area && area != 1 ? find_sprite(area, name) : 0;
    if (!s) s = find_sprite(1, name);
    if (s && sprite_info(s, p)) return 1;
    memset(p, 0, sizeof *p);
    p->builtin = builtin_of(name);
    if (!p->builtin) return 0;
    p->wos = p->builtin <= BI_RADIOON ? 40 : 68;
    p->hos = p->builtin <= BI_RADIOON ? 40 : 68;
    return 1;
}

static uint32_t sprite_rgb(const struct spr *p, int x, int y, int *solid)
{
    uint32_t row = p->img + (uint32_t)y * p->wwords * 4;
    *solid = 1;
    if (p->mask) {
        if (p->newmask) {
            uint32_t mrow = p->mask + (uint32_t)y * ((p->w + 31) / 32) * 4;
            *solid = MEM[mrow + x / 8] >> (x & 7) & 1;
        } else {
            int bit = p->first + x * p->bpp;
            uint32_t mrow = p->mask + (uint32_t)y * p->wwords * 4;
            *solid = (MEM[mrow + bit / 8] >> (bit & 7) & ((1 << p->bpp) - 1)) != 0;
        }
        if (!*solid) return 0;
    }
    if (p->bpp == 32) {
        const uint8_t *q = MEM + row + x * 4;
        return (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
    }
    if (p->bpp == 16) {
        int v = MEM[row + x * 2] | MEM[row + x * 2 + 1] << 8, r = v & 31, g = v >> 5 & 31, b = v >> 10 & 31;
        return (uint32_t)(r << 3 | r >> 2) << 16 | (uint32_t)(g << 3 | g >> 2) << 8 | (uint32_t)(b << 3 | b >> 2);
    }
    int bit = p->first + x * p->bpp;
    int v = MEM[row + bit / 8] >> (bit & 7) & ((1 << p->bpp) - 1);
    if (v < p->npal) {
        uint32_t c = rd(p->pal + v * 8);
        return (c >> 8 & 0xFF) << 16 | (c >> 16 & 0xFF) << 8 | c >> 24;
    }
    switch (p->bpp) {
    case 1: return v ? COL(7) : COL(0);
    case 2: { static const uint8_t m[4] = {0, 2, 4, 7}; return COL(m[v]); }
    case 4: return COL(v);
    default: {                            /* the default 256-colour palette */
        int t = v & 3;
        int r = (v >> 4 & 1) << 3 | (v >> 2 & 1) << 2 | t;
        int g = (v >> 6 & 1) << 3 | (v >> 5 & 1) << 2 | t;
        int b = (v >> 7 & 1) << 3 | (v >> 3 & 1) << 2 | t;
        return (uint32_t)(r * 17) << 16 | (uint32_t)(g * 17) << 8 | (uint32_t)(b * 17);
    }
    }
}

static void draw_builtin(int bi, int x, int y)   /* OS units: bottom left */
{
    int px = PX(x), py = PY(y + (bi <= BI_RADIOON ? 40 : 68));
    uint32_t k = COL(7), w = COL(0);
    switch (bi) {
    case BI_OPTOFF: case BI_OPTON:
        fill_px(px + 3, py + 3, px + 17, py + 17, w);
        frame_px(px + 3, py + 3, px + 17, py + 17, k);
        if (bi == BI_OPTON)                       /* a tick */
            for (int i = 0; i < 4; i++) {
                fill_px(px + 6 + i, py + 10 + i, px + 7 + i, py + 12 + i, k);
                fill_px(px + 9 + i, py + 12 - i * 1, px + 10 + i, py + 14 - i, k);
                fill_px(px + 13, py + 6 + i, px + 14, py + 8 + i, k);
            }
        break;
    case BI_RADIOOFF: case BI_RADIOON:
        for (int yy = 0; yy < 15; yy++)
            for (int xx = 0; xx < 15; xx++) {
                int dx = 2 * xx - 14, dy = 2 * yy - 14, d = dx * dx + dy * dy;
                if (d <= 196) dot(px + 3 + xx, py + 3 + yy, d >= 140 ? k : (bi == BI_RADIOON && d <= 40) ? k : w);
            }
        break;
    case BI_DIR:                                  /* a folder */
        fill_px(px + 3, py + 8, px + 14, py + 11, COL(14));
        frame_px(px + 3, py + 8, px + 14, py + 12, k);
        fill_px(px + 3, py + 11, px + 31, py + 28, COL(14));
        frame_px(px + 3, py + 11, px + 31, py + 28, k);
        break;
    case BI_APP:                                  /* a window */
        fill_px(px + 4, py + 6, px + 30, py + 28, w);
        fill_px(px + 4, py + 6, px + 30, py + 11, COL(15));
        frame_px(px + 4, py + 6, px + 30, py + 28, k);
        fill_px(px + 4, py + 11, px + 30, py + 12, k);
        break;
    case BI_FILE:                                 /* a page with a folded corner */
        fill_px(px + 8, py + 3, px + 26, py + 31, w);
        frame_px(px + 8, py + 3, px + 26, py + 31, k);
        for (int i = 0; i < 6; i++) { dot(px + 20 + i, py + 3 + i, k); dot(px + 20, py + 3 + i, k); dot(px + 20 + i, py + 9, k); }
        break;
    }
}

/* draw a sprite, bottom left at x, y (OS units); half: at half size;
 * effect 1 shaded (greyed), 2 selected (darkened) */
static void draw_sprite(const struct spr *p, int x, int y, int half, int effect)
{
    if (p->builtin) { draw_builtin(p->builtin, x, y); return; }
    int sh = half ? 1 : 0;
    int wos = p->wos >> sh, hos = p->hos >> sh;
    int px0 = PX(x), py0 = PY(y + hos), px1 = PX(x + wos), py1 = PY(y);
    for (int py = imax(py0, cl_y0); py < imin(py1, cl_y1); py++) {
        int sy = (((py - py0) * 2) << sh) >> p->yeig;
        if (sy >= p->h) continue;
        for (int px = imax(px0, cl_x0); px < imin(px1, cl_x1); px++) {
            int sx = (((px - px0) * 2) << sh) >> p->xeig, solid;
            if (sx >= p->w) continue;
            uint32_t c = sprite_rgb(p, sx, sy, &solid);
            if (!solid) continue;
            if (effect == 1) c = blend(c, COL(1));
            else if (effect == 2) c = (c >> 1) & 0x7F7F7F;
            fb[py * dw + px] = c;
        }
    }
}

/* ---------------------------------------------------------------- icons
 * Flags: 0 text, 1 sprite, 2 border, 3 centred horizontally, 4
 * vertically, 5 filled, 6 anti-aliased font, 8 indirected, 9 right
 * justified, 11 half-size sprite, 12-15 button type, 16-20 exclusive
 * selection group, 21 selected, 22 shaded, 23 deleted, 24-27 foreground
 * and 28-31 background colour. Indirected data: text buffer, validation
 * string, buffer size; for a sprite, its name (or the sprite itself if
 * the size is 0) and area. Validation: commands separated by ';' - S
 * sprite names (a second for when selected), D the character to display
 * in place of each one typed, R a border type, A the characters allowed. */
static int icon_text(const uint8_t *ic, char *out, int max)
{
    uint32_t f = wimp_rd(ic + 16);
    int n = 0;
    if (f & 0x100) {
        uint32_t buf = wimp_rd(ic + 20), len = wimp_rd(ic + 28);
        if ((f & 3) == 2) return 0;               /* indirected sprite only */
        if (len > (uint32_t)max - 1) len = (uint32_t)max - 1;
        while ((uint32_t)n < len && buf + n < MEMSIZE && MEM[buf + n] >= 32) { out[n] = MEM[buf + n]; n++; }
    } else
        while (n < 12 && ic[20 + n] >= 32) { out[n] = ic[20 + n]; n++; }
    out[n] = 0;
    return n;
}

static int validation(const uint8_t *ic, char cmd, char *out, int max)   /* a command's argument */
{
    uint32_t f = wimp_rd(ic + 16);
    if ((f & 0x101) != 0x101) return 0;
    uint32_t v = wimp_rd(ic + 24);
    if (!v || v == 0xFFFFFFFF || v >= MEMSIZE) return 0;
    char s[256]; int n = 0;
    while (n < 255 && v + n < MEMSIZE && MEM[v + n] >= 32) { s[n] = MEM[v + n]; n++; }
    s[n] = 0;
    for (char *p = s; *p; ) {
        char *e = p;
        while (*e && *e != ';') { if (*e == '\\' && e[1]) e++; e++; }
        if ((*p | 0x20) == (cmd | 0x20)) {
            int l = imin((int)(e - p - 1), max - 1);
            memcpy(out, p + 1, l); out[l] = 0;
            return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* the sprite an icon shows, if any */
static int icon_sprite(const uint8_t *ic, uint32_t area, struct spr *p)
{
    uint32_t f = wimp_rd(ic + 16);
    char name[64];
    if (!(f & 2)) return 0;
    if ((f & 0x103) == 0x102) {                   /* indirected sprite only */
        uint32_t a = wimp_rd(ic + 20), sa = wimp_rd(ic + 24), len = wimp_rd(ic + 28);
        if (len == 0) return sprite_info(a, p);
        int n = 0;
        while (n < 63 && (uint32_t)n < len && a + n < MEMSIZE && MEM[a + n] > 32) { name[n] = MEM[a + n]; n++; }
        name[n] = 0;
        return lookup_sprite(sa, name, p);
    }
    if (f & 0x100) {                              /* text and sprite: the S command */
        char s[64];
        if (!validation(ic, 'S', s, sizeof s)) return 0;
        char *comma = strchr(s, ',');
        if (comma) { *comma = 0; if (f & (1u << 21)) memmove(s, comma + 1, strlen(comma + 1) + 1); }
        return lookup_sprite(area, s, p);
    }
    int n = 0;
    while (n < 12 && ic[20 + n] > 32) { name[n] = ic[20 + n]; n++; }
    name[n] = 0;
    return lookup_sprite(area, name, p);
}

/* Where an icon's text and sprite go, x0..y1 its box on the screen */
struct icon_layout { int tx, ty, sx, sy, n; struct spr sp; int has_sp; char text[256]; };
static void layout_icon(const uint8_t *ic, int x0, int y0, int x1, int y1, uint32_t area, struct icon_layout *L)
{
    uint32_t f = wimp_rd(ic + 16);
    int half = (f >> 11) & 1;
    L->n = (f & 1) ? icon_text(ic, L->text, sizeof L->text) : 0;
    L->has_sp = icon_sprite(ic, area, &L->sp);
    int sw = L->has_sp ? L->sp.wos >> (L->sp.builtin ? 0 : half) : 0;
    int shh = L->has_sp ? L->sp.hos >> (L->sp.builtin ? 0 : half) : 0;
    int tw = L->n * CHAR_W, hc = f & 8, vc = f & 16, rj = f & 0x200;
    int cy = (y0 + y1) / 2;
    if (L->has_sp && L->n) {
        if (hc && !vc) {                          /* sprite above, text below (the icon bar's) */
            L->sx = (x0 + x1 - sw) / 2; L->sy = y1 - shh;
            L->tx = (x0 + x1 - tw) / 2; L->ty = y0 + CHAR_H;
        } else if (hc) {                          /* both in the middle */
            L->sx = (x0 + x1 - sw) / 2; L->sy = cy - shh / 2;
            L->tx = (x0 + x1 - tw) / 2; L->ty = cy + CHAR_H / 2;
        } else if (rj) {                          /* sprite at the right, text to its left */
            L->sx = x1 - sw; L->sy = cy - shh / 2;
            L->tx = x1 - sw - 8 - tw; L->ty = cy + CHAR_H / 2;
        } else {                                  /* sprite at the left, text to its right */
            L->sx = x0; L->sy = cy - shh / 2;
            L->tx = x0 + sw + 8; L->ty = cy + CHAR_H / 2;
        }
        return;
    }
    L->sx = hc ? (x0 + x1 - sw) / 2 : rj ? x1 - sw : x0;
    L->sy = vc ? cy - shh / 2 : y0;
    L->tx = hc ? (x0 + x1 - tw) / 2 : rj ? x1 - 12 - tw : x0 + 12;
    L->ty = cy + CHAR_H / 2;
}

static void slab(int x0, int y0, int x1, int y1, int in)   /* a 3D edge, 2 pixels, OS units */
{
    uint32_t lt = in ? COL(4) : COL(0), dk = in ? COL(0) : COL(4);
    int a = PX(x0), b = PY(y1), c = PX(x1), d = PY(y0);
    for (int k = 0; k < 2; k++) {
        fill_px(a + k, b + k, c - k, b + k + 1, lt); fill_px(a + k, b + k, a + k + 1, d - k, lt);
        fill_px(a + k, d - k - 1, c - k, d - k, dk); fill_px(c - k - 1, b + k, c - k, d - k, dk);
    }
}

static int bar_icons;                             /* drawing the icon bar's: text may be wider */

static void draw_icon(const uint8_t *ic, int ox, int oy, uint32_t area, int caret_index)
{
    uint32_t f = wimp_rd(ic + 16);
    if (f & (1u << 23)) return;
    int x0 = ox + rdi(ic), y0 = oy + rdi(ic + 4), x1 = ox + rdi(ic + 8), y1 = oy + rdi(ic + 12);
    int fg = f >> 24 & 15, bg = f >> 28 & 15, sel = f >> 21 & 1, shaded = f >> 22 & 1;
    if (f & 0x40) { fg = 7; bg = 1; }             /* an outline font's handle there instead */
    char r[8];
    int rtype = validation(ic, 'R', r, sizeof r) ? r[0] - '0' : -1;
    if (sel && !(f & 2)) { int t = fg; fg = bg; bg = t; }
    int c[4];
    clip_save(c);
    clip_and_os(bar_icons ? -0x10000 : x0, y0, bar_icons ? 0x10000 : x1, y1);
    if (f & 0x20) fill_os(x0, y0, x1, y1, COL(bg));
    if (rtype >= 1 && rtype <= 7) {               /* border types */
        int in = rtype == 2 || rtype == 4 || rtype == 7 || ((rtype == 5 || rtype == 6) && sel);
        if (rtype == 6) { frame_os(x0, y0, x1, y1, COL(7)); slab(x0 + 2, y0 + 2, x1 - 2, y1 - 2, in); }
        else slab(x0, y0, x1, y1, in);
    } else if (f & 4) frame_os(x0, y0, x1, y1, COL(fg));
    struct icon_layout L;
    layout_icon(ic, x0, y0, x1, y1, area, &L);
    if (L.has_sp) draw_sprite(&L.sp, L.sx, L.sy, (f >> 11) & 1, shaded ? 1 : (sel && !(f & 0x100)) ? 2 : 0);
    if (L.n) {
        char d[8];
        if (validation(ic, 'D', d, sizeof d)) memset(L.text, d[0], L.n);
        text_os(L.tx, L.ty, L.text, L.n, shaded ? COL(3) : COL(fg));
    }
    if (caret_index >= 0) {                       /* the caret: red, with serifs */
        int cx = PX(L.tx + caret_index * CHAR_W) - 1, top = PY(L.ty + 4), bot = PY(L.ty - CHAR_H - 4);
        fill_px(cx, top, cx + 1, bot, COL(11));
        fill_px(cx - 2, top, cx + 3, top + 1, COL(11));
        fill_px(cx - 2, bot - 1, cx + 3, bot, COL(11));
    }
    clip_restore(c);
}

/* the caret index nearest x (OS units) in a writable icon */
static int icon_index_at(const uint8_t *ic, int ox, int oy, uint32_t area, int x)
{
    struct icon_layout L;
    layout_icon(ic, ox + rdi(ic), oy + rdi(ic + 4), ox + rdi(ic + 8), oy + rdi(ic + 12), area, &L);
    int i = (x - L.tx + CHAR_W / 2) / CHAR_W;
    return i < 0 ? 0 : i > L.n ? L.n : i;
}

/* ---------------------------------------------------------------- windows */
enum { R_NONE, R_WORK, R_BACK, R_CLOSE, R_TITLE, R_TOGGLE, R_UP, R_VWELL, R_DOWN,
       R_ADJUST, R_LEFT, R_HWELL, R_RIGHT, R_FRAME };

struct geom {
    int vx0, vy0, vx1, vy1, sx, sy;               /* visible area, scroll offsets */
    int ox, oy;                                   /* the work area's origin on the screen */
    int o[4];                                     /* the outline */
    uint32_t f;                                   /* flags, new style */
    int furn;                                     /* any furniture at all */
    int ex0, ey0, ex1, ey1;                       /* work area extent */
};

static void geom_of(const struct win *w, struct geom *g)
{
    const uint8_t *b = w->blk;
    g->vx0 = rdi(b); g->vy0 = rdi(b + 4); g->vx1 = rdi(b + 8); g->vy1 = rdi(b + 12);
    g->sx = rdi(b + 16); g->sy = rdi(b + 20);
    g->ox = g->vx0 - g->sx; g->oy = g->vy1 - g->sy;
    g->ex0 = rdi(b + 40); g->ey0 = rdi(b + 44); g->ex1 = rdi(b + 48); g->ey1 = rdi(b + 52);
    uint32_t f = wimp_rd(b + 28);
    if (!(f & 0x80000000u))                       /* old-style flags */
        f = (f & 0xFFFF0072u) | (f & 1 ? 1u << 26 | 1u << 27 : 0) | (f & 4 ? 1u << 28 : 0) |
            (f & 8 ? 1u << 30 : 0) | (f & 0xC ? 1u << 29 : 0) | (f & 0x80 ? 0 : 3u << 24) | (f & 0x10);
    g->furn = b[32] != 0xFF;
    if (!g->furn) f &= 0x00FFFFFFu;
    if (!(f & (1u << 26))) f &= ~(3u << 24 | 1u << 27);   /* no title bar, no icons in it */
    g->f = f;
    g->o[0] = g->vx0 - (g->furn ? B : 0);
    g->o[1] = g->vy0 - (g->furn ? B : 0) - (f & (1u << 30) ? T + B : 0);
    g->o[2] = g->vx1 + (g->furn ? B : 0) + (f & (1u << 28) ? T + B : 0);
    g->o[3] = g->vy1 + (g->furn ? B : 0) + (f & (1u << 26) ? T + B : 0);
}

void desktop_outline(const struct win *w, int32_t *o)
{
    struct geom g;
    geom_of(w, &g);
    memcpy(o, g.o, sizeof g.o);
}

/* the furniture's parts, OS units (each with its border lines; neighbours
 * share one) */
struct part { int x0, y0, x1, y1; };
static int part_of(const struct geom *g, int r, struct part *p)
{
    uint32_t f = g->f;
    int vs = f & (1u << 28), hs = f & (1u << 30), adj = f & (1u << 29), title = f & (1u << 26);
    int L = g->o[0], R = g->o[2], Bt = g->o[1], Tp = g->o[3];
    int back = f & (1u << 24), close = f & (1u << 25), toggle = f & (1u << 27);
    int adj_v = adj && vs, adj_h = adj && hs && !vs;    /* where the adjust-size icon goes */
    int vbot = (hs ? g->vy0 - B : (adj_v ? g->vy0 - B + TB - B : g->vy0 - B));
    switch (r) {
    case R_TITLE: if (!title) return 0;
        *p = (struct part){L + (back ? TB - B : 0) + (close ? TB - B : 0), g->vy1, R - (toggle ? TB - B : 0), Tp}; return 1;
    case R_BACK: if (!back) return 0; *p = (struct part){L, g->vy1, L + TB, Tp}; return 1;
    case R_CLOSE: if (!close) return 0;
        *p = (struct part){L + (back ? TB - B : 0), g->vy1, L + (back ? TB - B : 0) + TB, Tp}; return 1;
    case R_TOGGLE: if (!toggle) return 0; *p = (struct part){R - TB, g->vy1, R, Tp}; return 1;
    case R_UP: if (!vs) return 0; *p = (struct part){g->vx1, g->vy1 + B - TB, R, g->vy1 + B}; return 1;
    case R_DOWN: if (!vs) return 0; *p = (struct part){g->vx1, vbot, R, vbot + TB}; return 1;
    case R_VWELL: if (!vs) return 0; *p = (struct part){g->vx1, vbot + TB - B, R, g->vy1 + B - TB + B}; return 1;
    case R_ADJUST:
        if (adj && vs && hs) { *p = (struct part){g->vx1, Bt, R, g->vy0}; return 1; }
        if (adj_v) { *p = (struct part){g->vx1, g->vy0 - B, R, g->vy0 - B + TB}; return 1; }
        if (adj_h) { *p = (struct part){g->vx1 + B - TB, Bt, g->vx1 + B, g->vy0}; return 1; }
        return 0;
    case R_LEFT: if (!hs) return 0; *p = (struct part){g->vx0 - B, Bt, g->vx0 - B + TB, g->vy0}; return 1;
    case R_RIGHT: if (!hs) return 0; {
        int rr = adj_h ? g->vx1 + B - TB + B : g->vx1 + B;
        *p = (struct part){rr - TB, Bt, rr, g->vy0}; return 1; }
    case R_HWELL: if (!hs) return 0; {
        int rr = adj_h ? g->vx1 + B - TB + B : g->vx1 + B;
        *p = (struct part){g->vx0 - B + TB - B, Bt, rr - TB + B, g->vy0}; return 1; }
    }
    return 0;
}

/* a scroll bar's slider within its well: [a, b) along it, OS units */
static int slider(const struct geom *g, int vertical, struct part *well, int *a, int *b)
{
    if (!part_of(g, vertical ? R_VWELL : R_HWELL, well)) return 0;
    int lo = vertical ? well->y0 + B : well->x0 + B, hi = vertical ? well->y1 - B : well->x1 - B;
    int len = hi - lo;
    int total = vertical ? g->ey1 - g->ey0 : g->ex1 - g->ex0;
    int vis = vertical ? g->vy1 - g->vy0 : g->vx1 - g->vx0;
    if (total <= 0 || vis >= total) { *a = lo; *b = hi; return 1; }
    int size = imax(len * vis / total, 16);
    int pos = vertical ? g->ey1 - g->sy : g->sx - g->ex0;   /* scrolled from the top / left */
    int start = (len - size) * imax(0, imin(pos, total - vis)) / (total - vis);
    if (vertical) { *b = hi - start; *a = *b - size; }
    else { *a = lo + start; *b = *a + size; }
    return 1;
}

static void glyph(int r, const struct part *p)   /* a furniture icon's picture */
{
    int x = PX(p->x0) + 1, y = PY(p->y1) + 1;    /* the inside: 20x20 pixels */
    uint32_t k = COL(7), w = COL(0);
    switch (r) {
    case R_BACK:
        frame_px(x + 3, y + 7, x + 12, y + 16, k);
        fill_px(x + 8, y + 3, x + 17, y + 12, w);
        frame_px(x + 8, y + 3, x + 17, y + 12, k);
        break;
    case R_CLOSE:
        for (int i = 0; i < 11; i++) {
            fill_px(x + 4 + i, y + 4 + i, x + 6 + i, y + 6 + i, k);
            fill_px(x + 14 - i, y + 4 + i, x + 16 - i, y + 6 + i, k);
        }
        break;
    case R_TOGGLE:
        frame_px(x + 3, y + 3, x + 17, y + 17, k);
        fill_px(x + 3, y + 3, x + 10, y + 10, k);
        break;
    case R_ADJUST:
        frame_px(x + 3, y + 3, x + 12, y + 12, k);
        fill_px(x + 7, y + 7, x + 17, y + 17, w);
        frame_px(x + 7, y + 7, x + 17, y + 17, k);
        break;
    case R_UP: case R_DOWN: case R_LEFT: case R_RIGHT:
        for (int i = 0; i < 6; i++) {             /* a triangle */
            int a = 10 - i, b2 = 10 + i;
            if (r == R_UP) fill_px(x + a - 1, y + 6 + i, x + b2, y + 7 + i, k);
            if (r == R_DOWN) fill_px(x + a - 1, y + 13 - i, x + b2, y + 14 - i, k);
            if (r == R_LEFT) fill_px(x + 6 + i, y + a - 1, x + 7 + i, y + b2, k);
            if (r == R_RIGHT) fill_px(x + 13 - i, y + a - 1, x + 14 - i, y + b2, k);
        }
        break;
    }
}

static void title_text(const struct win *w, char *out, int max)
{
    uint8_t ic[32];
    memcpy(ic + 16, w->blk + 56, 4);
    memcpy(ic + 20, w->blk + 72, 12);
    icon_text(ic, out, max);
}

/* what the program drew in it, from screen memory as it left it */
static void draw_user_picture(const struct win *w, const struct geom *g)
{
    if (!w->back || memcmp(w->back_vis, w->blk, 24)) return;
    const struct rg_mode *m = cur_mode();
    uint32_t lb = (uint32_t)m->width * m->bpp / 8, off = bank * mode_bytes(m);
    if (off + mode_bytes(m) > w->back_size) return;
    uint32_t pal[256];
    if (m->bpp <= 8) for (int k = 0; k < (1 << m->bpp); k++) pal[k] = vidc_rgb(256, (uint8_t)k);
    int c[4];
    clip_save(c);
    clip_and_os(g->vx0, g->vy0, g->vx1, g->vy1);
    for (int py = cl_y0; py < cl_y1; py++) {          /* (where the program drew it: back_dx/dy) */
        int cy = SH - 1 - 2 * py + w->back_dy;
        int my = m->height - 1 - (cy >> m->yeig);
        if (cy < 0 || my < 0 || my >= m->height) continue;
        const uint8_t *row = w->back + off + my * lb;
        for (int px = cl_x0; px < cl_x1; px++) {
            int cx = 2 * px + w->back_dx;
            int mx = cx >> m->xeig;
            if (cx < 0 || mx >= m->width) continue;
            uint32_t col;
            if (m->bpp == 32) col = (uint32_t)row[mx * 4] << 16 | (uint32_t)row[mx * 4 + 1] << 8 | row[mx * 4 + 2];
            else if (m->bpp == 16) {
                int v = row[mx * 2] | row[mx * 2 + 1] << 8, r = v & 31, gg = v >> 5 & 31, b = v >> 10 & 31;
                col = (uint32_t)(r << 3) << 16 | (uint32_t)(gg << 3) << 8 | (uint32_t)(b << 3);
            } else {
                int per = 8 / m->bpp, bit = (mx % per) * m->bpp;
                col = pal[row[mx / per] >> bit & ((1 << m->bpp) - 1)];
            }
            fb[py * dw + px] = col;
        }
    }
    clip_restore(c);
}

static void draw_window(const struct win *w)
{
    struct geom g;
    geom_of(w, &g);
    const uint8_t *b = w->blk;
    uint32_t area = wimp_rd(b + 64);
    int focus = wimp_caret[0] == w->handle;
    clip_all();
    /* the work area */
    if (b[35] != 0xFF) fill_os(g.vx0, g.vy0, g.vx1, g.vy1, COL(b[35]));
    if (!(g.f & 0x10)) draw_user_picture(w, &g);
    clip_os(g.vx0, g.vy0, g.vx1, g.vy1);
    for (int i = 0; i < w->nicons; i++)
        if (w->used[i])
            draw_icon(w->icon[i], g.ox, g.oy, area,
                      focus && wimp_caret[1] == (uint32_t)i ? (int)wimp_caret[5] : -1);
    if (focus && (int32_t)wimp_caret[1] < 0 && wimp_caret[4] != 0xFFFFFFFF) {   /* a caret of its own */
        int cx = PX(g.ox + (int32_t)wimp_caret[2]), cy = g.oy + (int32_t)wimp_caret[3];
        int h = (int)(wimp_caret[4] & 0xFFFF);
        fill_px(cx, PY(cy + h), cx + 1, PY(cy), COL(11));
    }
    clip_all();
    if (!g.furn) return;
    uint32_t k = COL(7);
    /* the frame around the work area */
    frame_os(g.vx0 - B, g.vy0 - B, g.vx1 + B, g.vy1 + B, k);
    struct part p;
    if (part_of(&g, R_TITLE, &p)) {
        uint32_t bg = COL(focus ? b[38] : b[33]);
        fill_os(p.x0, p.y0, p.x1, p.y1, bg);
        frame_os(p.x0, p.y0, p.x1, p.y1, k);
        char t[256];
        title_text(w, t, sizeof t);
        int n = (int)strlen(t), tw = n * CHAR_W;
        clip_os(p.x0 + B, p.y0 + B, p.x1 - B, p.y1 - B);
        text_os((p.x0 + p.x1 - tw) / 2, (p.y0 + p.y1) / 2 + CHAR_H / 2, t, n, COL(b[32]));
        clip_all();
    }
    static const int buttons[] = {R_BACK, R_CLOSE, R_TOGGLE, R_UP, R_DOWN, R_LEFT, R_RIGHT, R_ADJUST};
    for (unsigned i = 0; i < sizeof buttons / sizeof *buttons; i++)
        if (part_of(&g, buttons[i], &p)) {
            fill_os(p.x0, p.y0, p.x1, p.y1, COL(1));
            frame_os(p.x0, p.y0, p.x1, p.y1, k);
            glyph(buttons[i], &p);
        }
    for (int v = 0; v < 2; v++) {                 /* the scroll bars' wells and sliders */
        int a, s2;
        if (!slider(&g, v, &p, &a, &s2)) continue;
        fill_os(p.x0, p.y0, p.x1, p.y1, COL(b[36]));
        frame_os(p.x0, p.y0, p.x1, p.y1, k);
        if (v) { fill_os(p.x0 + B + 4, a + 2, p.x1 - B - 4, s2 - 2, COL(b[37])); frame_os(p.x0 + B + 4, a + 2, p.x1 - B - 4, s2 - 2, k); }
        else { fill_os(a + 2, p.y0 + B + 4, s2 - 2, p.y1 - B - 4, COL(b[37])); frame_os(a + 2, p.y0 + B + 4, s2 - 2, p.y1 - B - 4, k); }
    }
    frame_os(g.o[0], g.o[1], g.o[2], g.o[3], k);
}

static int in_rect(int x, int y, int x0, int y0, int x1, int y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }

/* the part of w under (x, y), and the icon if in its work area (-1 none) */
static int window_hit(const struct win *w, int x, int y, int *icon)
{
    struct geom g;
    geom_of(w, &g);
    *icon = -1;
    if (!in_rect(x, y, g.o[0], g.o[1], g.o[2], g.o[3])) return R_NONE;
    if (in_rect(x, y, g.vx0, g.vy0, g.vx1, g.vy1)) {
        for (int i = w->nicons - 1; i >= 0; i--) {
            const uint8_t *ic = w->icon[i];
            if (!w->used[i] || (wimp_rd(ic + 16) & (1u << 23))) continue;
            if (in_rect(x, y, g.ox + rdi(ic), g.oy + rdi(ic + 4), g.ox + rdi(ic + 8), g.oy + rdi(ic + 12))) { *icon = i; break; }
        }
        return R_WORK;
    }
    static const int parts[] = {R_BACK, R_CLOSE, R_TOGGLE, R_TITLE, R_UP, R_DOWN, R_VWELL, R_LEFT, R_RIGHT, R_HWELL, R_ADJUST};
    struct part p;
    for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++)
        if (part_of(&g, parts[i], &p) && in_rect(x, y, p.x0, p.y0, p.x1, p.y1)) return parts[i];
    return R_FRAME;
}

/* ---------------------------------------------------------------- the icon bar */
static int bar_dx[WIMP_MAXICON];                  /* each icon's x origin */

/* The icons are laid out from the ends of the bar inwards, each given
 * room for its text too (it may be wider than the icon: a name under it) */
static void layout_bar(void)
{
    struct win *w = &wimp_iconbar;
    int left = BAR_GAP, right = SW - BAR_GAP;
    for (int i = 0; i < w->nicons; i++) {
        if (!w->used[i]) continue;
        char t[256];
        int x0 = rdi(w->icon[i]), wd = rdi(w->icon[i] + 8) - x0;
        int tw = (wimp_rd(w->icon[i] + 16) & 1) ? icon_text(w->icon[i], t, sizeof t) * CHAR_W : 0;
        int pad = tw > wd ? (tw - wd + 1) / 2 : 0;
        if (wimp_bar_side[i] == -2) { bar_dx[i] = left + pad - x0; left += wd + 2 * pad + BAR_GAP; }
        else { right -= wd + pad; bar_dx[i] = right - x0; right -= pad + BAR_GAP; }
    }
}

static void draw_bar(void)
{
    struct win *w = &wimp_iconbar;
    clip_all();
    fill_os(0, 0, SW, BAR_H, COL(1));
    fill_os(0, BAR_H - B, SW, BAR_H, COL(7));
    clip_os(0, 0, SW, BAR_H - B);
    layout_bar();
    bar_icons = 1;
    for (int i = 0; i < w->nicons; i++)
        if (w->used[i]) draw_icon(w->icon[i], bar_dx[i], BAR_ORG, 1, -1);
    bar_icons = 0;
    clip_all();
}

static int bar_hit(int x, int y)                  /* an icon, -1 the bar, -2 not on it */
{
    struct win *w = &wimp_iconbar;
    if (y >= BAR_H) return -2;
    layout_bar();
    for (int i = 0; i < w->nicons; i++) {
        const uint8_t *ic = w->icon[i];
        if (w->used[i] && in_rect(x, y, bar_dx[i] + rdi(ic), BAR_ORG + rdi(ic + 4), bar_dx[i] + rdi(ic + 8), BAR_ORG + rdi(ic + 12)))
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- menus
 * A menu block: +0 title (12 bytes; indirected if the first item's flags
 * have bit 8: +0 the text), +12 title foreground, background, work area
 * foreground, background colours, +16 item width, +20 height, +24 gap,
 * then 24-byte items: +0 menu flags (0 ticked, 1 dotted line after, 2
 * writable, 3 warn rather than open the submenu, 7 last), +4 the submenu
 * (a menu, a window handle, or -1), +8 icon flags, +12 icon data.
 *
 * The open menus are levels, each a menu or a dialogue box (a window
 * opened as a menu), drawn over everything else. */
#define MAXLEVEL 8
static struct level {
    uint32_t menu;                                /* 0: a dialogue box */
    struct win *dbox;
    int x, y;                                     /* the items' top left */
    int w, ih, gap, n;                            /* item width (all of it), height, gap; count */
    int hover;                                    /* item under the pointer, -1 */
    int opened;                                   /* the item whose submenu is the next level, -1 */
} lv[MAXLEVEL];
static int nlv;
static int warned_level = -1, warned_item = -1;   /* Message_MenuWarning sent for */

static int item_top(const struct level *l, int i)
{
    int y = l->y;
    for (int k = 0; k < i; k++) y -= l->ih + l->gap + ((rd(l->menu + 28 + 24 * k) & 2) ? MENU_SEP : 0);
    return y;
}
static int menu_bottom(const struct level *l) { return item_top(l, l->n) + l->gap; }

static int menu_title(uint32_t m, char *out)
{
    int n = 0;
    if (rd(m + 28) & 0x100) {
        uint32_t t = rd(m);
        while (n < 100 && t + n < MEMSIZE && MEM[t + n] >= 32) { out[n] = MEM[t + n]; n++; }
    } else
        while (n < 12 && MEM[m + n] >= 32) { out[n] = MEM[m + n]; n++; }
    out[n] = 0;
    return n;
}

static void item_icon(const struct level *l, int i, uint8_t *ic)   /* as an icon block */
{
    memset(ic, 0, 16);
    memcpy(ic + 16, MEM + l->menu + 28 + 24 * i + 8, 16);
}

static void close_levels(int from)                /* close level from and those after it */
{
    for (int k = nlv - 1; k >= from; k--)
        if (lv[k].dbox) {
            if (wimp_menu_win == lv[k].dbox->handle) wimp_menu_win = 0;
            wimp_close(lv[k].dbox);
        }
    if (from < nlv) nlv = from;
    if (from > 0) lv[from - 1].opened = -1;
    if (warned_level >= from - 1) warned_level = warned_item = -1;
}

/* open a menu level with its items' top left at x, y - for a submenu that
 * doesn't fit to the right, ending at left instead (left of its parent) */
static void open_level(uint32_t menu, int x, int y, int left)
{
    if (nlv >= MAXLEVEL) return;
    struct level *l = &lv[nlv];
    memset(l, 0, sizeof *l);
    l->hover = l->opened = -1;
    struct win *w = wimp_window(menu);
    if (w && w != &wimp_iconbar) {                /* a dialogue box: its top left at x, y */
        uint8_t o[24];
        memcpy(o, w->blk, 24);
        int wd = rdi(o + 8) - rdi(o), ht = rdi(o + 12) - rdi(o + 4);
        struct geom g;
        geom_of(w, &g);
        int r = g.o[2] - g.vx1, t = g.o[3] - g.vy1, lft = g.vx0 - g.o[0], bot = g.vy0 - g.o[1];
        if (x + wd + r > SW && left > 0) x = left - r - wd;
        if (x + wd + r > SW) x = SW - r - wd;               /* kept on the screen */
        if (x - lft < 0) x = lft;
        if (y + t > SH) y = SH - t;
        if (y - ht - bot < 0) y = ht + bot;
        uint32_t v[4] = {(uint32_t)x, (uint32_t)(y - ht), (uint32_t)(x + wd), (uint32_t)y};
        memcpy(o, v, 16);
        l->dbox = w;
        nlv++;
        wimp_open(w, o, 0xFFFFFFFF);
        wimp_menu_win = w->handle;
        return;
    }
    if (menu + 28 > MEMSIZE) return;
    l->menu = menu;
    l->x = x; l->y = y;
    l->ih = (int)rd(menu + 20); l->gap = (int)rd(menu + 24);
    if (l->ih <= 0 || l->ih > 200) l->ih = 44;
    if (l->gap < 0 || l->gap > 100) l->gap = 0;
    char t[128];
    int wd = imax((int)rd(menu + 16), (menu_title(menu, t) + 1) * CHAR_W);
    for (int i = 0; i < 64 && menu + 28 + 24 * i + 24 <= MEMSIZE; i++) {
        l->n = i + 1;
        uint8_t ic[32];
        item_icon(l, i, ic);
        char s[256];
        wd = imax(wd, (icon_text(ic, s, sizeof s) + 1) * CHAR_W);
        if (rd(menu + 28 + 24 * i) & 0x80) break;
    }
    l->w = wd + MENU_TICK + MENU_ARROW;
    if (l->x + l->w + B > SW && left > 0) l->x = left - B - l->w;
    if (l->x + l->w + B > SW) l->x = SW - B - l->w;          /* kept on the screen */
    if (l->x < B) l->x = B;
    if (menu_bottom(l) - B < 0) l->y -= menu_bottom(l) - B;
    if (l->y + TB > SH) l->y = SH - TB;
    nlv++;
}

void desktop_create_menu(uint32_t menu, int x, int y)
{
    if (menu == 0xFFFFFFFF) { close_levels(0); return; }
    if (nlv && lv[0].menu && lv[0].menu == menu) return;   /* reopened (after Adjust): as it was */
    close_levels(0);
    open_level(menu, x, y, 0);
}

void desktop_create_submenu(uint32_t menu, int x, int y)
{
    if (warned_level < 0 || warned_level >= nlv) { desktop_create_menu(menu, x, y); return; }
    close_levels(warned_level + 1);
    lv[warned_level].opened = warned_item;
    open_level(menu, x, y, lv[warned_level].x - 2 * B);
}

void desktop_window_gone(struct win *w)
{
    for (int k = 0; k < nlv; k++)
        if (lv[k].dbox == w) { lv[k].dbox = NULL; close_levels(k); return; }
}

static void draw_menu(const struct level *l)
{
    uint32_t m = l->menu;
    int x0 = l->x, x1 = l->x + l->w, y1 = l->y, y0 = menu_bottom(l);
    uint32_t k = COL(7);
    clip_all();
    fill_os(x0, y1, x1, y1 + TB - B, COL(MEM[m + 13]));           /* the title */
    char t[128];
    int n = menu_title(m, t);
    clip_os(x0, y1, x1, y1 + TB - B);
    text_os(x0 + (l->w - n * CHAR_W) / 2, y1 + (TB - B) / 2 + CHAR_H / 2, t, n, COL(MEM[m + 12]));
    clip_all();
    fill_os(x0, y0, x1, y1, COL(MEM[m + 15]));
    for (int i = 0; i < l->n; i++) {
        uint32_t mf = rd(m + 28 + 24 * i), sub = rd(m + 28 + 24 * i + 4);
        uint8_t ic[32];
        item_icon(l, i, ic);
        uint32_t f = wimp_rd(ic + 16);
        int top = item_top(l, i), bot = top - l->ih - l->gap;
        int fg = f >> 24 & 15, bg = MEM[m + 15] & 15, shaded = f >> 22 & 1;
        if (i == l->hover && !shaded) { fill_os(x0, bot, x1, top, COL(fg)); int c = fg; fg = bg; bg = c; }
        char s[256];
        int len = icon_text(ic, s, sizeof s);
        int ty = (top + bot) / 2 + CHAR_H / 2;
        clip_os(x0, bot, x1, top);
        uint32_t tc = shaded ? COL(3) : COL(fg);
        if (len) text_os(x0 + MENU_TICK, ty, s, len, tc);
        else if (f & 2) {                         /* a sprite item */
            struct spr sp;
            if (icon_sprite(ic, 1, &sp)) draw_sprite(&sp, x0 + MENU_TICK, bot + (top - bot - sp.hos) / 2, 0, shaded);
        }
        if (mf & 1) {                             /* a tick */
            int px = PX(x0) + 2, py = PY(ty) + 8;
            for (int j = 0; j < 4; j++) { fill_px(px + j, py + j, px + j + 2, py + j + 2, tc); }
            for (int j = 0; j < 6; j++) { fill_px(px + 4 + j, py + 3 - j, px + 6 + j, py + 5 - j, tc); }
        }
        if (sub != 0xFFFFFFFF && sub != 0) {      /* a submenu arrow */
            int px = PX(x1 - MENU_ARROW / 2) - 2, py = PY(ty) + 8;
            for (int j = 0; j < 5; j++) fill_px(px + j, py - 4 + j, px + j + 1, py + 5 - j, tc);
        }
        clip_all();
        if (mf & 2)                               /* a dotted line below */
            for (int x = PX(x0) + 2; x < PX(x1) - 2; x += 2) dot(x, PY(bot - MENU_SEP / 2), COL(4));
    }
    frame_os(x0 - B, y0 - B, x1 + B, y1 + TB, k);
    fill_os(x0, y1, x1, y1 + B, k);
}

/* a menu level's item under (x, y): -1 none (on the title: -2) */
static int menu_hit(const struct level *l, int x, int y)
{
    if (!in_rect(x, y, l->x - B, menu_bottom(l) - B, l->x + l->w + B, l->y + TB)) return -1;
    if (y >= l->y) return -2;
    for (int i = 0; i < l->n; i++) {
        int top = item_top(l, i);
        if (y < top && y >= top - l->ih - l->gap) return i;
    }
    return -2;
}

/* ---------------------------------------------------------------- drawing it all */
static struct {                                   /* Wimp_DragBox (or 0) */
    int type, x0, y0, x1, y1, px0, py0, px1, py1, sx, sy;
} ubox;

static void render(void)
{
    fit_window();
    clip_all();
    fill_px(0, 0, dw, dh, COL(4));
    if (backdrop)
        memcpy(fb, backdrop, (size_t)dw * PY(BAR_H) * 4);
    draw_bar();
    struct win *st[WIMP_MAXWIN];
    int n = wimp_stack(st);
    for (int i = 0; i < n; i++) {
        int is_menu = 0;
        for (int k = 0; k < nlv; k++) if (lv[k].dbox == st[i]) is_menu = 1;
        if (!is_menu) draw_window(st[i]);
    }
    for (int k = 0; k < nlv; k++) {
        if (lv[k].dbox) draw_window(lv[k].dbox);
        else draw_menu(&lv[k]);
    }
    if (ubox.type == 5 || ubox.type == 6) {
        clip_all();
        dotted_px(PX(imin(ubox.x0, ubox.x1)), PY(imax(ubox.y0, ubox.y1)), PX(imax(ubox.x0, ubox.x1)) + 1,
                  PY(imin(ubox.y0, ubox.y1)) + 1, COL(7), (int)(monotonic / 4));
    }
}

/* shown pixel for pixel, never through a display shader */
void desktop_present(void)
{
    render();
    display_present_desktop(fb, dw, dh);
}

void desktop_screenshot(const char *path)
{
    render();
    display_screenshot_rgb(fb, dw, dh, path);
}

int desktop_size(int *w, int *h)
{
    if (!fb) fit_window();
    *w = SW; *h = SH;
    return fb != NULL;
}

/* ---------------------------------------------------------------- entering and leaving */
static int sprites_merged, auto_fullscreen;
int rg_windowed_games;
void desktop_enter(void)
{
    if (!sprites_merged) {                        /* the application's icons, as the Filer loads */
        sprites_merged = 1;                       /* them: !Sprites22 for square pixels, if there */
        char path[2048];
        DIR *d = opendir(rg_appdir);
        struct dirent *e;
        int best = 0;
        while (d && (e = readdir(d))) {               /* (a host name may add ",type" or ",load-exec") */
            size_t l = strcspn(e->d_name, ",");
            int which = l == 10 && !strncasecmp(e->d_name, "!Sprites22", l) ? 2 :
                        l == 8 && !strncasecmp(e->d_name, "!Sprites", l) ? 1 : 0;
            if (which > best) { best = which; snprintf(path, sizeof path, "%s/%s", rg_appdir, e->d_name); }
        }
        if (d) closedir(d);
        if (best) sprite_file_host(wimp_sprites(), path, 1);
    }
    if (visible) return;
    visible = 1;
    if (auto_fullscreen) { auto_fullscreen = 0; display_set_fullscreen(0); }
    if (cur_mode()->bpp <= 4)                     /* the Wimp's palette */
        for (int k = 0; k < 16; k++) vidc_palette[k] = vidc_palette_top[k] = wimp_vidc[k];
    vdu_put(5);                                   /* text at the graphics cursor, as in the desktop */
}

/* The program takes the screen for itself: fullscreen, unless the window
 * was already (or --windowed-games) - and back in the window when it
 * returns to the desktop */
void desktop_mode_changed(void)
{
    if (!visible) return;
    visible = 0;
    close_levels(0);
    if (rg_interactive && !rg_windowed_games && !display_fullscreen()) {
        auto_fullscreen = 1;
        display_set_fullscreen(1);
    }
}

/* ---------------------------------------------------------------- input */
static int prev_buttons;
static struct press {                             /* the button press being followed */
    int buttons, x, y;
    int kind;                                     /* P_... */
    struct win *w;
    int region, icon, level;
    int moved;                                    /* a drag has been reported */
    uint32_t t;                                   /* when (monotonic), for auto-repeat */
    uint8_t vis[24];                              /* the window's visible area and scroll then */
} pr;
enum { P_NONE, P_WORK, P_BAR, P_TITLE, P_ADJUST, P_VSLIDE, P_HSLIDE, P_ARROW, P_MENU };
static struct { struct win *w; int icon; int buttons; uint32_t t; } last_click;
static uint32_t last_open_sent;                   /* monotonic: one Open_Window_Request a tick */

static void event(uint32_t reason, const uint32_t *blk, int n) { roger_wimp_event(reason, blk, n); }

static void click_event(int x, int y, uint32_t b, uint32_t window, int icon)
{
    uint32_t blk[5] = {(uint32_t)x, (uint32_t)y, b, window, (uint32_t)icon};
    event(6, blk, 5);
}

static void open_request(struct win *w, const int32_t *vis, int32_t sx, int32_t sy, uint32_t behind)
{
    uint32_t blk[8] = {w->handle, (uint32_t)vis[0], (uint32_t)vis[1], (uint32_t)vis[2], (uint32_t)vis[3],
                       (uint32_t)sx, (uint32_t)sy, behind};
    event(2, blk, 8);
}

static uint32_t behind_now(struct win *w)         /* where it is in the stack: the window above, -1 */
{
    struct win *st[WIMP_MAXWIN];
    int n = wimp_stack(st);
    for (int i = 0; i + 1 < n; i++) if (st[i] == w) return st[i + 1]->handle;
    return 0xFFFFFFFF;
}

static void clamp_scroll(const struct geom *g, int *sx, int *sy)
{
    int vw = g->vx1 - g->vx0, vh = g->vy1 - g->vy0;
    if (*sx > g->ex1 - vw) *sx = g->ex1 - vw;
    if (*sx < g->ex0) *sx = g->ex0;
    if (*sy < g->ey0 + vh) *sy = g->ey0 + vh;
    if (*sy > g->ey1) *sy = g->ey1;
}

static void scroll_by(struct win *w, int dx, int dy, int page)   /* dx, dy: -1/+1 (x right, y up) */
{
    struct geom g;
    geom_of(w, &g);
    if (g.f & (3u << 8)) {                        /* the program scrolls it: Scroll_Request */
        uint32_t blk[10] = {w->handle};
        memcpy(blk + 1, w->blk, 24);
        blk[7] = behind_now(w);
        blk[8] = (uint32_t)(dx * (page ? 2 : 1)); blk[9] = (uint32_t)(dy * (page ? 2 : 1));
        event(10, blk, 10);
        return;
    }
    int sx = g.sx + dx * (page ? g.vx1 - g.vx0 : 32), sy = g.sy + dy * (page ? g.vy1 - g.vy0 : 32);
    clamp_scroll(&g, &sx, &sy);
    int32_t vis[4] = {g.vx0, g.vy0, g.vx1, g.vy1};
    open_request(w, vis, sx, sy, behind_now(w));
}

static void toggle_size(struct win *w)
{
    struct geom g;
    geom_of(w, &g);
    int32_t full[4] = {g.vx0, g.vy1 - (g.ey1 - g.ey0), g.vx0 + (g.ex1 - g.ex0), g.vy1};
    if (full[2] > SW - (g.o[2] - g.vx1)) { full[0] -= full[2] - (SW - (g.o[2] - g.vx1)); full[2] = SW - (g.o[2] - g.vx1); }
    if (full[0] < g.vx0 - g.o[0]) full[0] = g.vx0 - g.o[0];
    if (full[1] < BAR_H + (g.vy0 - g.o[1])) {
        full[1] = BAR_H + (g.vy0 - g.o[1]);
        full[3] = imin(full[1] + (g.ey1 - g.ey0), SH - (g.o[3] - g.vy1));
    }
    int is_full = g.vx0 == full[0] && g.vy0 == full[1] && g.vx1 == full[2] && g.vy1 == full[3];
    int32_t vis[4];
    if (is_full && w->full_prev[2] > w->full_prev[0]) memcpy(vis, w->full_prev, sizeof vis);
    else { memcpy(w->full_prev, &g.vx0, sizeof w->full_prev); memcpy(vis, full, sizeof vis); }
    int sx = g.sx, sy = g.sy;
    struct geom n = g;
    n.vx0 = vis[0]; n.vy0 = vis[1]; n.vx1 = vis[2]; n.vy1 = vis[3];
    clamp_scroll(&n, &sx, &sy);
    open_request(w, vis, sx, sy, 0xFFFFFFFF);
}

static int writable(const uint8_t *ic)
{
    uint32_t f = wimp_rd(ic + 16), t = f >> 12 & 15;
    return (t == 14 || t == 15) && (f & 0x101) == 0x101;
}

static void caret_to_icon(struct win *w, int icon, int x)
{
    struct geom g;
    geom_of(w, &g);
    int idx = icon_index_at(w->icon[icon], g.ox, g.oy, wimp_rd(w->blk + 64), x);
    uint32_t c[6] = {w->handle, (uint32_t)icon, (uint32_t)(x - g.ox), 0, 40, (uint32_t)idx};
    wimp_set_caret(c);
}

/* a press on an icon (or the work area, icon -1) by its button type */
static void work_press(struct win *w, uint32_t window, int icon, int x, int y, int b)
{
    uint32_t type;
    const uint8_t *ic = icon >= 0 ? w->icon[icon] : NULL;
    if (ic) type = wimp_rd(ic + 16) >> 12 & 15;
    else type = window == 0xFFFFFFFE ? 3 : wimp_rd(w->blk + 60) >> 12 & 15;
    if (ic && (wimp_rd(ic + 16) & (1u << 22))) return;     /* shaded */
    if (b == 2) { click_event(x, y, 2, window, icon); return; }   /* Menu: always */
    int dbl = last_click.w == w && last_click.icon == icon && last_click.buttons == b && monotonic - last_click.t < 50;
    last_click.w = w; last_click.icon = icon; last_click.buttons = b; last_click.t = monotonic;
    if (dbl) last_click.w = NULL;
    switch (type) {
    case 2: case 3: case 6: case 9: click_event(x, y, b, window, icon); break;
    case 5: case 8: if (dbl) click_event(x, y, b, window, icon); break;
    case 10: click_event(x, y, dbl ? b : b << 8, window, icon); break;
    case 11: wimp_auto_select(window, icon, b); click_event(x, y, b, window, icon); break;
    case 14: if (ic && writable(ic)) caret_to_icon(w, icon, x); click_event(x, y, b, window, icon); break;
    case 15: if (ic && writable(ic)) caret_to_icon(w, icon, x); break;
    }
}

/* the window under (x, y) (not the menus), its part and icon */
static struct win *window_at(int x, int y, int *region, int *icon)
{
    struct win *st[WIMP_MAXWIN];
    int n = wimp_stack(st);
    for (int i = n - 1; i >= 0; i--) {
        int is_menu = 0;
        for (int k = 0; k < nlv; k++) if (lv[k].dbox == st[i]) is_menu = 1;
        if (is_menu) continue;
        int r = window_hit(st[i], x, y, icon);
        if (r != R_NONE) { *region = r; return st[i]; }
    }
    *region = R_NONE; *icon = -1;
    return NULL;
}

/* the menu level under (x, y), -1 none; item: the item (-2 the title, -3 a
 * dialogue box) */
static int level_at(int x, int y, int *item)
{
    for (int k = nlv - 1; k >= 0; k--) {
        if (lv[k].dbox) {
            int ic;
            if (window_hit(lv[k].dbox, x, y, &ic) != R_NONE) { *item = -3; return k; }
        } else {
            int i = menu_hit(&lv[k], x, y);
            if (i != -1) { *item = i; return k; }
        }
    }
    return -1;
}

int desktop_pointer(int *x, int *y, int32_t *window, int32_t *icon)
{
    int item, region, ic;
    *x = mouse_x; *y = mouse_y;
    *window = -1; *icon = -1;
    int k = level_at(mouse_x, mouse_y, &item);
    if (k >= 0) {
        if (lv[k].dbox) {
            window_hit(lv[k].dbox, mouse_x, mouse_y, &ic);
            *window = (int32_t)lv[k].dbox->handle; *icon = ic;
        }
        return mouse_buttons;
    }
    struct win *w = window_at(mouse_x, mouse_y, &region, &ic);
    if (w) { *window = (int32_t)w->handle; *icon = ic; }
    else if ((ic = bar_hit(mouse_x, mouse_y)) != -2) { *window = -2; *icon = ic; }
    return mouse_buttons;
}

/* The pointer over a menu: the item under it is highlighted, and moving
 * onto an item's submenu arrow opens the submenu beside it - or, if the
 * item asks for it, sends Message_MenuWarning for the program to open it
 * (Wimp_CreateSubMenu) */
static void menu_hover(int x, int y)
{
    int item, k = level_at(x, y, &item);
    if (k < 0 || lv[k].dbox) return;
    struct level *l = &lv[k];
    int i = item >= 0 ? item : -1;
    if (i < 0) return;
    l->hover = i;
    if (l->opened >= 0 && l->opened != i) close_levels(k + 1);
    if (warned_level == k && warned_item != i) warned_level = warned_item = -1;
    uint32_t mf = rd(l->menu + 28 + 24 * i), sub = rd(l->menu + 28 + 24 * i + 4);
    uint32_t f = rd(l->menu + 28 + 24 * i + 8);
    if (sub == 0xFFFFFFFF || sub == 0 || (f & (1u << 22)) || l->opened == i) return;
    if (x < l->x + l->w - MENU_ARROW - 16) return;
    int sx = l->x + l->w + 2 * B, sy = item_top(l, i);
    if (mf & 8) {
        if (warned_level == k && warned_item == i) return;
        uint32_t blk[8 + MAXLEVEL + 2] = {0, 0, 0, 0, 0x400C0, sub, (uint32_t)sx, (uint32_t)sy};
        int n = 8;
        for (int j = 0; j < k; j++) blk[n++] = (uint32_t)lv[j].opened;
        blk[n++] = (uint32_t)i;
        blk[n++] = 0xFFFFFFFF;
        blk[0] = 4 * n;
        warned_level = k; warned_item = i;
        event(17, blk, n);
        return;
    }
    close_levels(k + 1);
    l->opened = i;
    open_level(sub, sx, sy, l->x - 2 * B);
}

static void arrow(struct win *w, int region, int b)
{
    int d = b == 1 ? -1 : 1;                      /* Adjust: the other way */
    if (region == R_UP) scroll_by(w, 0, d, 0);
    else if (region == R_DOWN) scroll_by(w, 0, -d, 0);
    else if (region == R_LEFT) scroll_by(w, -d, 0, 0);
    else scroll_by(w, d, 0, 0);
}

static void press(int x, int y, int b)
{
    int item, k = level_at(x, y, &item);
    if (nlv && k < 0) { close_levels(0); return; }   /* a click away from the menus closes them */
    if (k >= 0 && !lv[k].dbox) {                  /* a menu item chosen: Menu_Selection */
        if (item < 0) return;
        if (rd(lv[k].menu + 28 + 24 * item + 8) & (1u << 22)) return;   /* shaded */
        uint32_t blk[MAXLEVEL + 1];
        int n = 0;
        for (int j = 0; j < k; j++) blk[n++] = (uint32_t)lv[j].opened;
        blk[n++] = (uint32_t)item;
        blk[n++] = 0xFFFFFFFF;
        if (b == 4) close_levels(0);              /* Select closes the menu; Adjust keeps it */
        event(9, blk, n);
        return;
    }
    struct win *w;
    int region, icon;
    if (k >= 0) { w = lv[k].dbox; region = window_hit(w, x, y, &icon); }
    else w = window_at(x, y, &region, &icon);
    memset(&pr, 0, sizeof pr);
    pr.buttons = b; pr.x = x; pr.y = y; pr.t = monotonic;
    if (!w) {
        int i = bar_hit(x, y);
        if (i >= 0) { pr.kind = P_BAR; pr.w = &wimp_iconbar; pr.icon = i; work_press(&wimp_iconbar, 0xFFFFFFFE, i, x, y, b); }
        return;
    }
    pr.w = w; pr.region = region; pr.icon = icon;
    memcpy(pr.vis, w->blk, 24);
    struct geom g;
    geom_of(w, &g);
    int32_t vis[4] = {g.vx0, g.vy0, g.vx1, g.vy1};
    switch (region) {
    case R_WORK: pr.kind = P_WORK; work_press(w, w->handle, icon, x, y, b); break;
    case R_TITLE:
        if (b == 2) break;
        if (b == 4) open_request(w, vis, g.sx, g.sy, 0xFFFFFFFF);   /* Select: to the front */
        if (g.f & 2) pr.kind = P_TITLE;                            /* moveable: drag it */
        break;
    case R_BACK: if (b != 2) open_request(w, vis, g.sx, g.sy, 0xFFFFFFFE); break;
    case R_CLOSE: if (b != 2) { uint32_t h = w->handle; event(3, &h, 1); } break;
    case R_TOGGLE: if (b != 2) toggle_size(w); break;
    case R_ADJUST: if (b != 2) pr.kind = P_ADJUST; break;
    case R_UP: case R_DOWN: case R_LEFT: case R_RIGHT:
        if (b != 2) { arrow(w, region, b); pr.kind = P_ARROW; }
        break;
    case R_VWELL: case R_HWELL: {
        if (b == 2) break;
        struct part p;
        int a, s2, v = region == R_VWELL, pos = v ? y : x;
        if (!slider(&g, v, &p, &a, &s2)) break;
        if (pos >= a && pos < s2) { pr.kind = v ? P_VSLIDE : P_HSLIDE; break; }
        int dir = pos >= s2 ? 1 : -1;
        if (b == 1) dir = -dir;
        scroll_by(w, v ? 0 : dir, v ? dir : 0, 1);
        break;
    }
    }
}

static int drag_type(int type) { return type == 6 || type == 7 || type == 8 || type == 10 || type == 11 || type == 14; }

static void held(int x, int y)
{
    int dx = x - pr.x, dy = y - pr.y;
    struct win *w = pr.w;
    if (!w) return;
    if (pr.kind == P_WORK && !pr.moved && (dx * dx + dy * dy > 12 * 12)) {
        uint32_t type = pr.icon >= 0 ? wimp_rd(w->icon[pr.icon] + 16) >> 12 & 15 : wimp_rd(w->blk + 60) >> 12 & 15;
        pr.moved = 1;
        if (drag_type((int)type) && pr.buttons != 2) click_event(pr.x, pr.y, (uint32_t)pr.buttons << 4, w->handle, pr.icon);
        return;
    }
    if (pr.kind == P_ARROW && monotonic - pr.t > 30) {     /* auto-repeat */
        arrow(w, pr.region, pr.buttons);
        pr.t = monotonic - 25;
        return;
    }
    if (pr.kind < P_TITLE || pr.kind > P_HSLIDE || monotonic == last_open_sent) return;
    struct geom g;
    struct win orig = *w;
    memcpy(orig.blk, pr.vis, 24);
    geom_of(&orig, &g);
    int32_t vis[4] = {g.vx0, g.vy0, g.vx1, g.vy1};
    int sx = g.sx, sy = g.sy;
    switch (pr.kind) {
    case P_TITLE: {
        if (!(g.f & 0x40)) {                      /* kept on the screen */
            if (g.o[0] + dx < 0) dx = -g.o[0];
            if (g.o[2] + dx > SW) dx = SW - g.o[2];
            if (g.o[3] + dy > SH) dy = SH - g.o[3];
            if (g.o[1] + dy < 0) dy = -g.o[1];
        }
        vis[0] += dx; vis[2] += dx; vis[1] += dy; vis[3] += dy;
        break;
    }
    case P_ADJUST: {
        int minw = w->blk[68] | w->blk[69] << 8, minh = w->blk[70] | w->blk[71] << 8;
        if (!minw) minw = 2 * TB;
        if (!minh) minh = 2 * TB;
        vis[2] = imax(vis[0] + minw, imin(vis[2] + dx, vis[0] + (g.ex1 - g.ex0)));
        vis[1] = imin(vis[3] - minh, imax(vis[1] + dy, vis[3] - (g.ey1 - g.ey0)));
        struct geom n = g;
        n.vx1 = vis[2]; n.vy0 = vis[1];
        clamp_scroll(&n, &sx, &sy);
        break;
    }
    case P_VSLIDE: case P_HSLIDE: {
        struct part p;
        int v = pr.kind == P_VSLIDE, a, s2;
        if (!slider(&g, v, &p, &a, &s2)) return;
        int len = v ? p.y1 - p.y0 - 2 * B : p.x1 - p.x0 - 2 * B, size = s2 - a;
        int total = v ? g.ey1 - g.ey0 : g.ex1 - g.ex0, vsz = v ? g.vy1 - g.vy0 : g.vx1 - g.vx0;
        if (len <= size || total <= vsz) return;
        if (v) sy -= dy * (total - vsz) / (len - size);
        else sx += dx * (total - vsz) / (len - size);
        clamp_scroll(&g, &sx, &sy);
        break;
    }
    }
    if (!memcmp(vis, w->blk, 16) && sx == rdi(w->blk + 16) && sy == rdi(w->blk + 20)) return;
    open_request(w, vis, sx, sy, behind_now(w));
    last_open_sent = monotonic;
}

static void release(int x, int y)
{
    if (pr.kind == P_WORK && pr.icon >= 0 && !pr.moved && pr.buttons != 2) {
        uint32_t type = wimp_rd(pr.w->icon[pr.icon] + 16) >> 12 & 15;
        int region, icon, item;
        struct win *w = level_at(x, y, &item) >= 0 ? NULL : window_at(x, y, &region, &icon);
        int k = level_at(x, y, &item);
        if (k >= 0 && lv[k].dbox) { w = lv[k].dbox; window_hit(w, x, y, &icon); }
        if ((type == 4 || type == 7) && w == pr.w && icon == pr.icon)   /* released over it */
            click_event(x, y, (uint32_t)pr.buttons, pr.w->handle, pr.icon);
    }
    pr.kind = P_NONE;
}

/* Wimp_DragBox: R1 -> window, drag type, box, parent box (0: cancel). A
 * user drag (types 5 fixed size, 6 rubber box, 7 a point - and the
 * system's, taken as 7) follows the pointer within the parent box until
 * the buttons are released: User_Drag_Box gives the final box. */
void desktop_drag_box(uint32_t blk)
{
    if ((int32_t)blk <= 0) { ubox.type = 0; return; }
    ubox.type = (int)rd(blk + 4);
    if (ubox.type < 5 || ubox.type > 7) ubox.type = 7;
    ubox.x0 = (int32_t)rd(blk + 8); ubox.y0 = (int32_t)rd(blk + 12);
    ubox.x1 = (int32_t)rd(blk + 16); ubox.y1 = (int32_t)rd(blk + 20);
    ubox.px0 = (int32_t)rd(blk + 24); ubox.py0 = (int32_t)rd(blk + 28);
    ubox.px1 = (int32_t)rd(blk + 32); ubox.py1 = (int32_t)rd(blk + 36);
    ubox.sx = mouse_x; ubox.sy = mouse_y;
    pr.kind = P_NONE;
}

static void drag_box_input(int x, int y, int b)
{
    int dx = x - ubox.sx, dy = y - ubox.sy;
    ubox.sx = x; ubox.sy = y;
    if (ubox.type == 6) { ubox.x1 += dx; ubox.y1 += dy; }
    else {
        if (ubox.x0 + dx < ubox.px0) dx = ubox.px0 - ubox.x0;
        if (ubox.x1 + dx > ubox.px1) dx = ubox.px1 - ubox.x1;
        if (ubox.y0 + dy < ubox.py0) dy = ubox.py0 - ubox.y0;
        if (ubox.y1 + dy > ubox.py1) dy = ubox.py1 - ubox.y1;
        ubox.x0 += dx; ubox.x1 += dx; ubox.y0 += dy; ubox.y1 += dy;
    }
    if (!b) {
        uint32_t blk[4] = {(uint32_t)ubox.x0, (uint32_t)ubox.y0, (uint32_t)ubox.x1, (uint32_t)ubox.y1};
        ubox.type = 0;
        event(7, blk, 4);
    }
}

/* ---------------------------------------------------------------- keys
 * Typing goes to the window with the caret: in a writable icon the Wimp
 * edits the text itself (characters its A validation allows, Delete and
 * Backspace, Copy, the cursor left and right, Ctrl-U to clear it); the
 * rest reach the program as Key_Pressed, in the Wimp's codes (function
 * and cursor keys &180 on, +&10 with Shift, +&20 with Ctrl). Escape closes
 * the menus if open. */
static int allowed(const uint8_t *ic, int ch)
{
    char a[128];
    if (!validation(ic, 'A', a, sizeof a)) return 1;
    int incl = 1, ok = a[0] == '~';
    for (char *p = a; *p; p++) {
        if (*p == '~') { incl = !incl; continue; }
        int lo = (*p == '\\' && p[1]) ? (uint8_t)*++p : (uint8_t)*p, hi = lo;
        if (p[1] == '-' && p[2]) { p += 2; hi = (*p == '\\' && p[1]) ? (uint8_t)*++p : (uint8_t)*p; }
        if (ch >= lo && ch <= hi) ok = incl;
    }
    return ok;
}

static void key(int code)
{
    if (nlv && code == 27) { close_levels(0); return; }
    struct win *w = wimp_window(wimp_caret[0]);
    if (!w || w == &wimp_iconbar) return;
    int icon = (int32_t)wimp_caret[1];
    if (icon >= 0 && icon < w->nicons && w->used[icon] && writable(w->icon[icon])) {
        const uint8_t *ic = w->icon[icon];
        uint32_t buf = wimp_rd(ic + 20), len = wimp_rd(ic + 28), n = 0;
        while (n < len && buf + n < MEMSIZE && MEM[buf + n] >= 32) n++;
        uint32_t idx = wimp_caret[5] > n ? n : wimp_caret[5];
        int done = 1;
        if ((code >= 32 && code < 127) || (code >= 128 && code < 256)) {
            if (allowed(ic, code) && n + 1 < len) {
                memmove(MEM + buf + idx + 1, MEM + buf + idx, n - idx + 1);
                MEM[buf + idx++] = (uint8_t)code;
            }
        } else if (code == 127 || code == 8) {
            if (idx > 0) { memmove(MEM + buf + idx - 1, MEM + buf + idx, n - idx + 1); idx--; }
        } else if (code == 0x18B) {
            if (idx < n) memmove(MEM + buf + idx, MEM + buf + idx + 1, n - idx);
        } else if (code == 0x18C) { if (idx > 0) idx--; }
        else if (code == 0x18D) { if (idx < n) idx++; }
        else if (code == 21) { MEM[buf] = 13; idx = 0; }
        else done = 0;
        if (done) { wimp_caret[5] = idx; return; }
    }
    uint32_t blk[7];
    memcpy(blk, wimp_caret, 24);
    blk[6] = (uint32_t)code;
    event(8, blk, 7);
}

static void keys(void)
{
    static const struct { uint8_t k; uint16_t code; } special[] = {
        {25, 0x18C}, {121, 0x18D}, {41, 0x18E}, {57, 0x18F}, {105, 0x18B},
        {113, 0x181}, {114, 0x182}, {115, 0x183}, {20, 0x184}, {116, 0x185}, {117, 0x186},
        {22, 0x187}, {118, 0x188}, {119, 0x189}, {30, 0x1CA}, {28, 0x1CB}, {29, 0x1CC},
        {61, 0x1CD}, {63, 0x19F}, {78, 0x19E}, {62, 0x1E},
    };
    static uint8_t was[128];
    for (unsigned i = 0; i < sizeof special / sizeof *special; i++) {
        int k = special[i].k;
        if (key_down[k] && !was[k]) {
            int code = special[i].code;
            if (code >= 0x180) code += (key_down[0] ? 0x10 : 0) + (key_down[1] ? 0x20 : 0);
            key(code);
        }
        was[k] = key_down[k];
    }
    if (escape_pressed) { escape_pressed = 0; key(27); }
    int c;
    while ((c = key_buffer_get()) >= 0) key(c == 9 ? 0x18A : c);
}

/* ---------------------------------------------------------------- the user */
void desktop_input(void)
{
    fit_window();
    int x = mouse_x, y = mouse_y, b = mouse_buttons;
    keys();
    if (ubox.type) { drag_box_input(x, y, b); prev_buttons = b; return; }
    if (nlv && !b) menu_hover(x, y);
    int down = b & ~prev_buttons, up = prev_buttons & ~b;
    if (pr.kind && (b & pr.buttons)) held(x, y);
    if (pr.kind && (up & pr.buttons)) release(x, y);
    for (int bit = 4; bit; bit >>= 1)
        if (down & bit) press(x, y, bit);
    prev_buttons = b;
}

void desktop_wait(void)
{
    if (rg_interactive) { display_sleep_ns(2000000); rt_service(0); }
    else roger_idle(2000);
}
