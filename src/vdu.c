/* The VDU drivers and sprite plotting, enough for games that draw through
 * the OS: MODE, CLS/CLG, COLOUR/GCOL and tints, graphics windows, origin,
 * PLOT (move, lines, points, triangles, rectangles), text (VDU 4 at the
 * text cursor in a text window, VDU 5 at the graphics cursor; VDU 23
 * characters; the character set is font.c's, public domain, not the
 * ROM's) and OS_SpriteOp 8/9/10/11/24/28/34 on user sprite areas and the
 * system sprite area, in any of the standard graphics modes. Used in
 * interactive runs (and with --full-os); headless runs only record the VDU
 * stream. */
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

static const uint8_t vdu_params[32] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 5, 0, 0, 1, 9, 8, 5, 0, 0, 4, 4, 0, 2};

static uint8_t qbuf[16];
static int qwant, qhave;
static int gfg_col = 63, gbg_col = 0, gfg_act, gbg_act, tfg_col = 63, tbg_col = 0;
static int tint_tfg = 3, tint_tbg, tint_gfg = 3, tint_gbg;
static int ox, oy;                              /* graphics origin (OS units) */
static int wl, wb, wr = 319, wt = 255;          /* graphics window (pixels, y down) */
static int cx, cy, px_, py_;                    /* last two points (absolute OS units) */
unsigned vdu_unhandled;

/* text: the character set (VDU 23 redefines it), VDU 4 / VDU 5, the text
 * cursor and window in character cells (x right, y down) */
extern const uint8_t roger_font[256][8];
static uint8_t chars[256][8];
static uint32_t chosen_sprite;                   /* for PLOT &E8-&EF (*SChoose, OS_SpriteOp 24) */
static void put_sprite(uint32_t s, int x, int y, int action);
static int chars_set, vdu5;
static int tcx, tcy, twl, twt, twr = 39, twb = 31;

static const struct rg_mode *md(void) { return cur_mode(); }
static int scr_w(void) { return md()->width; }
static int scr_h(void) { return md()->height; }
static uint32_t line_bytes(void) { return (uint32_t)md()->width * md()->bpp / 8; }
static uint8_t *bankmem(void) { return MEM + write_bank_addr(); }

/* GCOL/COLOUR colour number (+ tint) -> pixel value. In 256-colour modes
 * the colour number is bbggrr (6 bits) plus a 2-bit tint, laid out in the
 * pixel as VIDC uses it (see vidc_rgb()). */
static uint8_t colour_pixel(int c, int tint)
{
    if (md()->bpp == 8) {
        c &= 63;
        return (uint8_t)(((c & 1) << 2) | (((c >> 1) & 1) << 4) | (((c >> 2) & 1) << 5) |
                         (((c >> 3) & 1) << 6) | (((c >> 4) & 1) << 3) | (((c >> 5) & 1) << 7) |
                         (tint & 3));
    }
    return (uint8_t)(c & ((1 << md()->bpp) - 1));
}

/* pixels within a row: leftmost in the lowest bits */
static int get_px(const uint8_t *row, int x, int bpp)
{
    if (bpp == 8) return row[x];
    if (bpp == 16) return row[2 * x] | row[2 * x + 1] << 8;
    if (bpp == 32) return (int)(row[4 * x] | row[4 * x + 1] << 8 | row[4 * x + 2] << 16 | (uint32_t)row[4 * x + 3] << 24);
    int per = 8 / bpp, sh = (x % per) * bpp;
    return (row[x / per] >> sh) & ((1 << bpp) - 1);
}
static void put_px(uint8_t *row, int x, int bpp, int v)
{
    if (bpp == 8) { row[x] = (uint8_t)v; return; }
    if (bpp >= 16) { for (int k = 0; k < bpp / 8; k++) row[bpp / 8 * x + k] = (uint8_t)(v >> 8 * k); return; }
    int per = 8 / bpp, sh = (x % per) * bpp, m = ((1 << bpp) - 1) << sh;
    row[x / per] = (uint8_t)((row[x / per] & ~m) | ((v << sh) & m));
}
static int apply(int act, int old, int c, int bpp)
{
    int mask = bpp >= 32 ? -1 : (1 << bpp) - 1;
    switch (act & 7) {
    case 0: return c;
    case 1: return old | c;
    case 2: return old & c;
    case 3: return old ^ c;
    case 4: return ~old & mask;
    default: return old;
    }
}

static void pset(int x, int y, int fg)          /* pixel coords, y down */
{
    if (x < wl || x > wr || y < wb || y > wt) return;
    uint8_t *row = bankmem() + y * line_bytes();
    int bpp = md()->bpp;
    int act = fg ? gfg_act : gbg_act;
    int c = fg ? colour_pixel(gfg_col, tint_gfg) : colour_pixel(gbg_col, tint_gbg);
    put_px(row, x, bpp, apply(act, get_px(row, x, bpp), c, bpp));
}
/* for the Font Manager (fonts.c): a pixel value at pixel coordinates (y
 * down), inside the graphics window; the graphics origin; a colour number's
 * pixel value */
void vdu_paint_pixel(int x, int y, int v)
{
    if (x < wl || x > wr || y < wb || y > wt) return;
    put_px(bankmem() + y * line_bytes(), x, md()->bpp, v);
}
void vdu_origin(int *x, int *y) { *x = ox; *y = oy; }
void vdu_text_cursor(int *x, int *y) { *x = tcx - twl; *y = tcy - twt; }   /* in the text window */
uint8_t vdu_colour_pixel(int c) { return colour_pixel(c, 0); }

static int tox(int x) { return x >> md()->xeig; }
static int toy(int y) { return scr_h() - 1 - (y >> md()->yeig); }

static void hline(int x0, int x1, int y, int fg)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) pset(x, y, fg);
}

static void line(int x0, int y0, int x1, int y1, int fg)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pset(x0, y0, fg);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* PLOT &80-&8F, flood fill (BASIC's FILL is PLOT 133): from the point,
 * across the pixels of the background colour (&80-&87) or those that
 * aren't the foreground colour (&88-&8F), within the graphics window */
static void flood(int x, int y, int to_fg, int fg)
{
    if (x < wl || x > wr || y < wb || y > wt) return;
    int W = md()->width, H = scr_h(), bpp = md()->bpp;
    int bgv = colour_pixel(gbg_col, tint_gbg), fgv = colour_pixel(gfg_col, tint_gfg);
    uint8_t *seen = calloc((size_t)W * H, 1);
    int *stack = malloc(sizeof(int) * 4 * (size_t)W * H), n = 0;   /* (a run is pushed twice at most) */
    if (!seen || !stack) { free(seen); free(stack); return; }
#define ROW(Y) (bankmem() + (Y) * line_bytes())
#define FILLS(X, Y) (!seen[(Y) * W + (X)] && \
                     (to_fg ? get_px(ROW(Y), X, bpp) != fgv : get_px(ROW(Y), X, bpp) == bgv))
    stack[n++] = x; stack[n++] = y;
    while (n) {
        int yy = stack[--n], xx = stack[--n];
        if (!FILLS(xx, yy)) continue;
        int l = xx, r = xx;                      /* the run of the line it's in */
        while (l > wl && FILLS(l - 1, yy)) l--;
        while (r < wr && FILLS(r + 1, yy)) r++;
        for (int k = l; k <= r; k++) seen[yy * W + k] = 1;
        for (int d = -1; d <= 1; d += 2) {       /* the runs above and below */
            int ny = yy + d;
            if (ny < wb || ny > wt) continue;
            for (int k = l; k <= r; k++)
                if (FILLS(k, ny) && (k == l || !FILLS(k - 1, ny))) { stack[n++] = k; stack[n++] = ny; }
        }
        for (int k = l; k <= r; k++) pset(k, yy, fg);
    }
#undef FILLS
#undef ROW
    free(seen);
    free(stack);
}

static void triangle(int x0, int y0, int x1, int y1, int x2, int y2, int fg)
{
    int t;                                       /* sort by y, scan-convert */
    if (y0 > y1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y0 > y2) { t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
    if (y1 > y2) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    for (int y = y0; y <= y2; y++) {
        int xa = y2 == y0 ? x0 : x0 + (x2 - x0) * (y - y0) / (y2 - y0), xb;
        if (y < y1) xb = y1 == y0 ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else xb = y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        hline(xa, xb, y, fg);
    }
}

/* An ellipse (OS units): centre, horizontal and vertical semi-axes, shear
 * (x offset per unit of y), outline or filled - one pixel row at a time.
 * An outline joins each row's ends to the previous row's; its pixels are
 * collected first and each plotted once (EOR plotting would cancel any
 * plotted twice). */
static void mark_line(uint8_t *bm, int w, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        bm[y0 * w + x0] = 1;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void ellipse(int xc, int yc, int a, int b, double shear, int fill, int fg)
{
    int ye = md()->yeig, top = toy(yc + b), bot = toy(yc - b);
    if (b == 0) { hline(tox(xc - a), tox(xc + a), toy(yc), fg); return; }
    int rows = bot - top + 1, n = 0;
    int *L = malloc(rows * sizeof *L), *Rr = malloc(rows * sizeof *Rr);
    int xmin = 1 << 30, xmax = -(1 << 30);
    for (int py = top; py <= bot; py++, n++) {
        int dy = ((scr_h() - 1 - py) << ye) + (1 << ye) / 2 - yc;
        if (dy > b) dy = b;
        if (dy < -b) dy = -b;
        double w = a * sqrt(1.0 - (double)dy * dy / ((double)b * b)), m = xc + shear * dy;
        L[n] = tox((int)lround(m - w)); Rr[n] = tox((int)lround(m + w));
        if (L[n] < xmin) xmin = L[n];
        if (Rr[n] > xmax) xmax = Rr[n];
    }
    if (fill) for (int k = 0; k < rows; k++) hline(L[k], Rr[k], top + k, fg);
    else {
        int w = xmax - xmin + 1;
        uint8_t *bm = calloc((size_t)w * rows, 1);
        for (int k = 0; k < rows; k++) {
            if (k == 0 || k == rows - 1) for (int x = L[k]; x <= Rr[k]; x++) bm[k * w + x - xmin] = 1;
            else {
                mark_line(bm, w, L[k - 1] - xmin, k - 1, L[k] - xmin, k);
                mark_line(bm, w, Rr[k - 1] - xmin, k - 1, Rr[k] - xmin, k);
            }
        }
        if (rows > 1) {                          /* the last row joined to the one above */
            mark_line(bm, w, L[rows - 2] - xmin, rows - 2, L[rows - 1] - xmin, rows - 1);
            mark_line(bm, w, Rr[rows - 2] - xmin, rows - 2, Rr[rows - 1] - xmin, rows - 1);
        }
        for (int k = 0; k < rows; k++)
            for (int x = 0; x < w; x++) if (bm[k * w + x]) pset(xmin + x, top + k, fg);
        free(bm);
    }
    free(L); free(Rr);
}

static void plot(int k, int x, int y)
{
    if (!(k & 4)) { x += cx; y += cy; }          /* relative */
    else { x += ox; y += oy; }
    int mode = k & 3, grp = k & ~7;
    int fg = mode != 3;
    int saved = gfg_act;
    if (mode == 2) gfg_act = 4;                  /* inverse */
    if (mode != 0) {
        int X0 = tox(cx), Y0 = toy(cy), X1 = tox(x), Y1 = toy(y);
        if (grp <= 56) line(X0, Y0, X1, Y1, fg);
        else if (grp == 64) pset(X1, Y1, fg);
        else if (grp == 80) triangle(tox(px_), toy(py_), X0, Y0, X1, Y1, fg);
        else if (grp == 128 || grp == 136) flood(X1, Y1, grp == 136, fg);
        else if (grp == 232) {                     /* PLOT &E8-&EF: the chosen sprite, its */
            if (chosen_sprite)                     /* bottom left at the point */
                put_sprite(chosen_sprite, x - ox, y - oy, gfg_act);
        } else if (grp == 96) {
            int ya = Y0 < Y1 ? Y0 : Y1, yb = Y0 < Y1 ? Y1 : Y0;
            for (int yy = ya; yy <= yb; yy++) hline(X0, X1, yy, fg);
        } else if (grp == 144 || grp == 152) {     /* circle: centre the current point */
            int r = (int)lround(hypot(x - cx, y - cy));
            ellipse(cx, cy, r, r, 0, grp == 152, fg);
        } else if (grp == 192 || grp == 200) {     /* ellipse: centre the previous point, */
            int b = y - py_;                       /* the current one level with it (the */
            ellipse(px_, py_, abs(cx - px_), abs(b),   /* width), this one the top (height, */
                    b ? (double)(x - px_) / b : 0, grp == 200, fg);   /* shear) */
        } else vdu_unhandled++;
    }
    gfg_act = saved;
    px_ = cx; py_ = cy; cx = x; cy = y;
}

static int s16(int i) { return (int16_t)(qbuf[i] | qbuf[i + 1] << 8); }

static void clear_rect(int x0, int y0, int x1, int y1, int c)
{
    int bpp = md()->bpp;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) put_px(bankmem() + y * line_bytes(), x, bpp, c);
}

static void reset_windows(void)
{
    wl = 0; wb = 0; wr = scr_w() - 1; wt = scr_h() - 1; ox = oy = 0;
    twl = 0; twt = 0; twr = scr_w() / 8 - 1; twb = scr_h() / 8 - 1; tcx = tcy = 0;
}

/* ------------------------------------------------------------------ text */
static int char_w(void) { return 8 << md()->xeig; }     /* in OS units */
static int char_h(void) { return 8 << md()->yeig; }

static void text_scroll(void)                   /* the text window up a line */
{
    int bpp = md()->bpp, bg = colour_pixel(tbg_col, tint_tbg);
    int x0 = twl * 8, x1 = twr * 8 + 7, y0 = twt * 8, y1 = twb * 8 + 7;
    for (int y = y0; y <= y1; y++) {
        uint8_t *dst = bankmem() + y * line_bytes();
        const uint8_t *src = bankmem() + (y + 8) * line_bytes();
        for (int x = x0; x <= x1; x++) put_px(dst, x, bpp, y + 8 <= y1 ? get_px(src, x, bpp) : bg);
    }
}

static void text_down(void)
{
    if (++tcy > twb) { tcy = twb; text_scroll(); }
}

static void text_cell(int c, int col, int row)  /* VDU 4: an opaque cell */
{
    int bpp = md()->bpp, fg = colour_pixel(tfg_col, tint_tfg), bg = colour_pixel(tbg_col, tint_tbg);
    if (col < 0 || row < 0 || (col + 1) * 8 > scr_w() || (row + 1) * 8 > scr_h()) return;
    for (int r = 0; r < 8; r++) {
        uint8_t *dst = bankmem() + (row * 8 + r) * line_bytes();
        for (int b = 0; b < 8; b++) put_px(dst, col * 8 + b, bpp, chars[c][r] & (0x80 >> b) ? fg : bg);
    }
}

/* OS_Word 10: a character's definition, as VDU 23 sets it */
void vdu_char_def(int c, uint8_t *def)
{
    if (!chars_set) { memcpy(chars, roger_font, sizeof chars); chars_set = 1; }
    memcpy(def, chars[c & 255], 8);
}

static void put_char(int c)
{
    if (!chars_set) { memcpy(chars, roger_font, sizeof chars); chars_set = 1; }
    if (vdu5) {                                  /* at the graphics cursor, its top left */
        if (c == 127) { cx -= char_w(); return; }
        int x0 = tox(cx), y0 = toy(cy);
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (chars[c][r] & (0x80 >> b)) pset(x0 + b, y0 + r, 1);
        cx += char_w();
        return;
    }
    if (c == 127) {                              /* delete: back one and blank it */
        if (tcx > twl) tcx--;
        text_cell(' ', tcx, tcy);
        return;
    }
    text_cell(c, tcx, tcy);
    if (++tcx > twr) { tcx = twl; text_down(); }
}

/* cursor control codes, for VDU 4 or VDU 5 text: 1 if handled */
static int text_control(int code)
{
    if (vdu5) {
        switch (code) {
        case 8: cx -= char_w(); return 1;
        case 9: cx += char_w(); return 1;
        case 10: cy -= char_h(); return 1;
        case 11: cy += char_h(); return 1;
        case 13: cx = wl << md()->xeig; return 1;
        case 30: cx = wl << md()->xeig; cy = ((scr_h() - 1 - wb) << md()->yeig) + (1 << md()->yeig) - 1; return 1;
        case 31: cx = (twl + qbuf[0]) * char_w();
                 cy = (scr_h() << md()->yeig) - 1 - (twt + qbuf[1]) * char_h(); return 1;
        }
        return 0;
    }
    switch (code) {
    case 8: if (--tcx < twl) { tcx = twr; if (tcy > twt) tcy--; } return 1;
    case 9: if (++tcx > twr) { tcx = twl; text_down(); } return 1;
    case 10: text_down(); return 1;
    case 11: if (tcy > twt) tcy--; return 1;
    case 13: tcx = twl; return 1;
    case 30: tcx = twl; tcy = twt; return 1;
    case 31: if (twl + qbuf[0] <= twr && twt + qbuf[1] <= twb) { tcx = twl + qbuf[0]; tcy = twt + qbuf[1]; }
             return 1;
    }
    return 0;
}
static void reset_colours(void)
{
    int top = md()->bpp == 8 ? 63 : (1 << md()->bpp) - 1;
    tfg_col = gfg_col = top; tbg_col = gbg_col = 0; gfg_act = gbg_act = 0;
    tint_tfg = tint_gfg = 3; tint_tbg = tint_gbg = 0;
}

/* after a mode change (video_set_mode): the new mode's screen is cleared,
 * as RISC OS's CLS does, and the rest of screen memory keeps what it had
 * (a program double-buffering in it may never draw some of its second
 * bank: Enter the Realm's MODE 12 CLS in colour 7, then MODE 9, leaves
 * its play area's edges and last rows colour 7 in both) */
void vdu_mode_reset(void)
{
    uint32_t n = mode_bytes(md());
    reset_windows(); reset_colours();
    memset(MEM + screen_base(), 0, n < screen_size() ? n : screen_size()); cx = cy = px_ = py_ = 0;
    vdu5 = 0;
}

static void do_vdu(int code)
{
    if (text_control(code)) return;
    switch (code) {
    case 4: vdu5 = 0; break;
    case 5: vdu5 = 1; break;
    case 12:                                     /* CLS: the text window (CLG with VDU 5) */
        if (vdu5) { clear_rect(wl, wb, wr, wt, colour_pixel(gbg_col, tint_gbg)); text_control(30); }
        else { clear_rect(twl * 8, twt * 8, twr * 8 + 7, twb * 8 + 7, colour_pixel(tbg_col, tint_tbg));
               tcx = twl; tcy = twt; }
        break;
    case 16: clear_rect(wl, wb, wr, wt, colour_pixel(gbg_col, tint_gbg)); break;
    case 17: { int c = qbuf[0]; if (c & 128) tbg_col = c & 63; else tfg_col = c & 63; break; }
    case 18: { int a = qbuf[0], c = qbuf[1];
               if (c & 128) { gbg_act = a; gbg_col = c & 63; } else { gfg_act = a; gfg_col = c & 63; }
               break; }
    case 20: reset_colours(); break;
    case 22: break;                              /* MODE: video_set_mode(), from vdu_put() */
    case 23:
        if (qbuf[0] >= 32) {                     /* define a character */
            if (!chars_set) { memcpy(chars, roger_font, sizeof chars); chars_set = 1; }
            memcpy(chars[qbuf[0]], qbuf + 1, 8);
        }
        if (qbuf[0] == 17 && qbuf[1] <= 3) {     /* VDU 23,17,n,tint: set tint */
            int t = qbuf[2] >> 6;
            if (qbuf[1] == 0) tint_tfg = t; else if (qbuf[1] == 1) tint_tbg = t;
            else if (qbuf[1] == 2) tint_gfg = t; else tint_gbg = t;
        }
        break;
    case 24: {                                   /* graphics window, OS units incl. origin */
        int l = tox(s16(0) + ox), b = toy(s16(2) + oy), r = tox(s16(4) + ox), t = toy(s16(6) + oy);
        wl = l < 0 ? 0 : l; wr = r > scr_w() - 1 ? scr_w() - 1 : r;
        wb = t < 0 ? 0 : t; wt = b > scr_h() - 1 ? scr_h() - 1 : b;
        break;
    }
    case 25: plot(qbuf[0], s16(1), s16(3)); break;
    case 26: reset_windows(); cx = cy = px_ = py_ = 0; break;   /* also homes the graphics cursor */
    case 28: {                                   /* text window: left, bottom, right, top */
        int l = qbuf[0], b = qbuf[1], r = qbuf[2], t = qbuf[3];
        if (l <= r && t <= b && r < scr_w() / 8 && b < scr_h() / 8) {
            twl = l; twb = b; twr = r; twt = t; tcx = l; tcy = t;
        }
        break;
    }
    case 29: ox = s16(0); oy = s16(2); break;
    default: break;                              /* 19 palette, ... */
    }
}

void vdu_write(uint8_t c)
{
    if (qwant) {
        qbuf[qhave++] = c;
        if (qhave == qwant) { qwant = 0; do_vdu(qbuf[15]); }
        return;
    }
    if (c < 32) {
        if (vdu_params[c]) { qwant = vdu_params[c]; qhave = 0; qbuf[15] = c; }
        else do_vdu(c);
        return;
    }
    put_char(c);
}

/* ------------------------------------------------------------------ sprites
 * Sprite area: +4 count, +8 first sprite. Sprite: +0 next, +4 name (12),
 * +16 width in words - 1, +20 height - 1, +24 first bit used, +28 last bit
 * used, +32 image offset, +36 mask offset (= image: no mask). Sprites are
 * taken to be in the current mode's pixel format. */
static uint32_t rd32(uint32_t a) { return *(uint32_t *)(MEM + a); }

static uint32_t find_sprite(uint32_t area, uint32_t name)
{
    char want[13] = {0};
    for (int k = 0; k < 12; k++) {
        uint8_t c = MEM[name + k];
        if (c <= 32) break;
        want[k] = c;
    }
    uint32_t n = rd32(area + 4), s = area + rd32(area + 8);
    for (uint32_t k = 0; k < n; k++) {
        char nm[13] = {0};
        memcpy(nm, MEM + s + 4, 12);
        if (!strcasecmp(nm, want)) return s;
        s += rd32(s);
    }
    return 0;
}

static uint32_t rdp(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* a sprite (sp: its header, in emulated memory or a host buffer) */
static void put_sprite_at(const uint8_t *sp, int x, int y, int action)
{
    int bpp = md()->bpp;
    int wwords = rdp(sp + 16) + 1, h = rdp(sp + 20) + 1;
    int first = rdp(sp + 24), last = rdp(sp + 28);
    const uint8_t *img = sp + rdp(sp + 32), *mask = sp + rdp(sp + 36);
    int hasmask = mask != img && (action & 8);
    int wpx = (wwords * 32 - first - (31 - last)) / bpp;
    int skip = first / bpp;
    int left = tox(x + ox), top = toy(y + oy) - (h - 1);
    for (int r = 0; r < h; r++) {
        int yy = top + r;
        if (yy < wb || yy > wt) continue;
        const uint8_t *src = img + r * wwords * 4;
        const uint8_t *msk = mask + r * wwords * 4;
        uint8_t *dst = bankmem() + yy * line_bytes();
        for (int c = 0; c < wpx; c++) {
            int xx = left + c;
            if (xx < wl || xx > wr) continue;
            if (hasmask && !get_px(msk, skip + c, bpp)) continue;
            put_px(dst, xx, bpp, apply(action, get_px(dst, xx, bpp), get_px(src, skip + c, bpp), bpp));
        }
    }
}
static void put_sprite(uint32_t s, int x, int y, int action) { put_sprite_at(MEM + s, x, y, action); }

/* a sprite's pixel depth and eigen factors, from its mode word: a mode
 * number, or a RISC OS 3.5 sprite type (bits 27-31) with its dpi */
static void sprite_mode(uint32_t mw, int *bpp, int *xe, int *ye)
{
    static const int type_bpp[8] = {0, 1, 2, 4, 8, 16, 32, 0};
    if (mw < 256) {
        const struct rg_mode *m = mode_info(mw);
        if (!m) m = md();
        *bpp = m->bpp; *xe = m->xeig; *ye = m->yeig;
        return;
    }
    *bpp = type_bpp[(mw >> 27) & 7] ? type_bpp[(mw >> 27) & 7] : 8;
    *xe = ((mw >> 1) & 0x1FFF) <= 45 ? 2 : ((mw >> 1) & 0x1FFF) <= 90 ? 1 : 0;
    *ye = ((mw >> 14) & 0x1FFF) <= 45 ? 2 : ((mw >> 14) & 0x1FFF) <= 90 ? 1 : 0;
}

/* OS_SpriteOp 52 (put sprite scaled): at x, y (OS units), a sprite pixel
 * to a screen pixel scaled by the factors at fac (x mul, y mul, x div, y
 * div; 0: none), and its colours through the table at tab (one byte a
 * colour; 0: none), from a sprite of any depth to the screen's */
static void put_sprite_scaled(uint32_t s, int x, int y, int action, uint32_t fac, uint32_t tab)
{
    const uint8_t *sp = MEM + s;
    int sbpp, sxe, sye, bpp = md()->bpp;
    sprite_mode(rdp(sp + 40), &sbpp, &sxe, &sye);
    int wwords = rdp(sp + 16) + 1, h = rdp(sp + 20) + 1, first = rdp(sp + 24), last = rdp(sp + 28);
    const uint8_t *img = sp + rdp(sp + 32), *mask = sp + rdp(sp + 36);
    int hasmask = mask != img && (action & 8);
    int w = (wwords * 32 - first - (31 - last)) / sbpp, skip = first / sbpp;
    if (wwords <= 0 || wwords > 4096 || h <= 0 || h > 4096 || w <= 0
        || s + rdp(sp + 32) + (uint32_t)(wwords * 4 * h) > MEMSIZE) {
        ev("put_sprite_scaled_bad", "iiii", s, (uint32_t)wwords, (uint32_t)h, rdp(sp + 40));
        return;
    }
    int64_t xm = 1, ym = 1, xd = 1, yd = 1;
    if (fac) { xm = rd32(fac); ym = rd32(fac + 4); xd = rd32(fac + 8); yd = rd32(fac + 12); }
    if (xd <= 0 || yd <= 0 || xm <= 0 || ym <= 0) return;
    int dw = (int)((int64_t)w * xm / xd), dh = (int)((int64_t)h * ym / yd);   /* (screen pixels: the
                                                    factors allow for the modes, as Wimp_ReadPixTrans's do) */
    int left = tox(x + ox), top = toy(y + oy) - (dh - 1);
    for (int r = 0; r < dh; r++) {
        int yy = top + r;
        if (yy < wb || yy > wt) continue;
        int sr = (int)((int64_t)r * h / dh);
        const uint8_t *src = img + sr * wwords * 4, *msk = mask + sr * wwords * 4;
        uint8_t *dst = bankmem() + yy * line_bytes();
        for (int c = 0; c < dw; c++) {
            int xx = left + c;
            if (xx < wl || xx > wr) continue;
            int sc = skip + (int)((int64_t)c * w / dw);
            if (hasmask && !get_px(msk, sc, sbpp)) continue;
            int v = get_px(src, sc, sbpp);
            if (tab && sbpp <= 8) v = MEM[tab + v];
            put_px(dst, xx, bpp, apply(action, get_px(dst, xx, bpp), v & ((1 << bpp) - 1), bpp));
        }
    }
}

/* *ScreenLoad (OS_SpriteOp 3): a sprite file's first sprite, in its own
 * mode (changed to if it isn't the current one), with its palette if it
 * has one (the first flash colours; 16 at most, as VIDC has), at the
 * bottom left of the screen. f: the file, n bytes (an area without its
 * size word). 0 if it holds no sprite. */
int vdu_screenload(const uint8_t *f, size_t n)
{
    if (n < 12 || rdp(f) == 0) return 0;
    uint32_t first = rdp(f + 4);
    if (first < 4 || first - 4 + 44 > n) return 0;
    const uint8_t *sp = f + first - 4;
    uint32_t next = rdp(sp), imgoff = rdp(sp + 32);
    if (first - 4 + next > n || imgoff < 44 || imgoff > next) return 0;
    uint32_t mode = rdp(sp + 40);
    const struct rg_mode *m = mode < 256 ? mode_info(mode) : NULL;
    if (m && (m->width != md()->width || m->height != md()->height || m->bpp != md()->bpp)) {
        vdu_write(22);
        vdu_write((uint8_t)mode);
    }
    for (uint32_t k = 0; k < (imgoff - 44) / 8 && k < 16; k++) {
        uint32_t c = rdp(sp + 44 + 8 * k);           /* &BBGGRR00 */
        uint8_t v[6] = {19, (uint8_t)k, 16, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
        for (int j = 0; j < 6; j++) vdu_write(v[j]);
    }
    if (rg_full_os) put_sprite_at(sp, -ox, -oy, 0);
    return 1;
}

/* *SChoose: a sprite of the system area, for PLOT &E8-&EF; 0 if none */
int vdu_schoose(const char *name)
{
    uint32_t n = SCRATCH;
    snprintf((char *)MEM + n, 13, "%s", name);
    chosen_sprite = find_sprite(sys_sprites(), n);
    return chosen_sprite != 0;
}

/* OS_SpriteOp 9/24/28/34/36 on a user area or the system area: 1 if handled */
int sprite_op_full(void)
{
    uint32_t op = R[0] & 0xFF, area = sprite_area();
    uint32_t spr = 0;
    if ((R[0] & 0x300) != 0x200) spr = (op >= 24) ? find_sprite(area, R[2]) : 0;
    else spr = R[2];
    if (op >= 24 && !spr) { os_error(0x86, "Sprite doesn't exist"); return 1; }
    switch (op) {
    case 40: {                                   /* read sprite information */
        const struct rg_mode *m = mode_info(rd32(spr + 40));
        int bpp = m ? m->bpp : 1;
        uint32_t bits = (rd32(spr + 16) + 1) * 32 - rd32(spr + 24) - (31 - rd32(spr + 28));
        R[3] = bits / bpp;
        R[4] = rd32(spr + 20) + 1;
        R[5] = rd32(spr + 36) != rd32(spr + 32);
        R[6] = rd32(spr + 40);
        return 1;
    }
    case 9:                                      /* initialise area */
        *(uint32_t *)(MEM + area + 4) = 0;
        *(uint32_t *)(MEM + area + 8) = 16;
        *(uint32_t *)(MEM + area + 12) = 16;
        return 1;
    case 24:                                     /* select sprite (for PLOT &E8-&EF too) */
        if (spr) { R[2] = spr; chosen_sprite = spr; }
        return 1;
    case 28:                                     /* put sprite at graphics cursor */
        if (spr && rg_full_os) put_sprite(spr, cx - ox, cy - oy, R[5]);
        return 1;
    case 34:                                     /* put sprite at user coordinates */
        if (spr && rg_full_os) put_sprite(spr, (int32_t)R[3], (int32_t)R[4], R[5]);
        return 1;
    case 36: {                                   /* set pointer shape: R3 bits 0-3 the shape, */
        /* bit 4 set: not selected (OS_Byte 106), 5: not its palette, 6: not its pixels; R4, R5
         * the active point. Pixel values beyond 3 (more than 2 bits a pixel) keep their low bits */
        struct pointer_def *d = &pointer_defs[R[3] & 15];
        if (!(R[3] & 0x40)) {
            const struct rg_mode *m = mode_info(rd32(spr + 40));
            int bpp = m ? m->bpp : 1, stride = (rd32(spr + 16) + 1) * 4, first = rd32(spr + 24);
            uint32_t bits = (rd32(spr + 16) + 1) * 32 - first - (31 - rd32(spr + 28));
            d->w = bits / bpp > 32 ? 32 : bits / bpp;
            d->h = rd32(spr + 20) + 1 > 64 ? 64 : rd32(spr + 20) + 1;
            d->ax = R[4]; d->ay = R[5];
            for (int y = 0; y < d->h; y++)
                for (int x = 0; x < d->w; x++) {
                    int b = first + x * bpp;
                    d->pix[y * 32 + x] = MEM[spr + rd32(spr + 32) + y * stride + b / 8] >> b % 8 & 3;
                }
        }
        if (!(R[3] & 0x20) && rd32(spr + 32) > 44)   /* its palette's colours 1-3 */
            for (int k = 1; k < 4; k++) {
                uint32_t e = rd32(spr + 44 + 8 * k);
                pointer_rgb[k] = (e >> 8 & 0xFF) << 16 | (e >> 16 & 0xFF) << 8 | e >> 24;
            }
        pointer_gen++;
        if (!(R[3] & 0x10)) pointer_shape = R[3] & 15;
        return 1;
    }
    case 52:                                     /* put sprite scaled: R6 factors, R7 table */
        if (spr && rg_full_os) put_sprite_scaled(spr, (int32_t)R[3], (int32_t)R[4], R[5], R[6], R[7]);
        return 1;
    default:
        return 0;
    }
}
