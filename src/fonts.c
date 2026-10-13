/* The Font Manager, for RISC OS 2's pixel fonts: a font directory along
 * Font$Prefix (or Font$Path) holding IntMetrics (the characters' widths)
 * and x90y45 (anti-aliased bitmaps at a few sizes). A font is found at any
 * size and resolution - the screen mode's by default - and painted from the
 * nearest larger bitmap size, scaled (an area average of its 16 levels).
 * Each level is painted in the closest colour the screen has to its share
 * of the way from the background colour to the foreground: the logical
 * colours' own, or the physical ones Font_SetPalette gave for that
 * foreground. Level 0 is transparent. Outline fonts (RISC OS 3's Outlines,
 * f9999x9999 files) aren't read.
 *
 * x90y45: an index of 16-byte entries (point size, bits per pixel (4),
 * x dpi, y dpi, a checksum, the block's offset, its length; a zero point
 * size ends it), and per block: x size (1/16 pt * x dpi), y size, x dpi,
 * y dpi, a word, then 256 16-bit offsets (from the table) of the
 * characters, each signed x0 and y0 (pixels from the origin to its bottom
 * left), width, height, and its pixels bottom row first, 4 bits each, low
 * nibble first. IntMetrics: the name (40 bytes, padded with CRs), 16, 16,
 * the number of characters n (byte 48), at 52 a 256-byte map from
 * character codes to indices, then six arrays of n 16-bit values: x0, y0,
 * x1, y1 (the bounding box) and the x and y offsets to the next character,
 * in 1/1000 em. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define MAXFONT 32

struct glyph { int x0, y0, w, h; uint8_t *lv; };     /* scaled: pixels, y up; levels 0-15 */

static struct font {
    char name[64];
    int xsize, ysize, xres, yres, uses;              /* 1/16 pt; dpi */
    uint8_t *metrics, *bitmaps;
    long nbitmaps;
    uint32_t block;                                  /* the bitmap size scaled from */
    double sx, sy;                                   /* its scale to this size */
    struct glyph g[256];
    uint8_t done[256];
} fonts[MAXFONT + 1];                                /* handles 1-MAXFONT */

static int current, f_bg, f_fg = 7, f_off = 14;      /* Font_SetFontColours */
static struct { int set; uint32_t bg, fg; } pal[256];   /* Font_SetPalette, by foreground colour */

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    uint8_t *b = malloc(*n > 0 ? *n : 1);
    *n = (long)fread(b, 1, *n, f);
    fclose(f);
    return b;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int rd16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

/* a font's file along Font$Prefix or Font$Path (each a directory, or a
 * list of them separated by commas): name "Trinity.Medium" */
static uint8_t *font_file(const char *name, const char *leaf, long *n)
{
    const char *vars[] = {"Font$Prefix", "Font$Path"};
    for (int v = 0; v < 2; v++) {
        const char *p = roger_getvar(vars[v]);
        while (p && *p) {
            const char *e = strchr(p, ',');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            char riscos[512], host[2048];
            snprintf(riscos, sizeof riscos, "%.*s%s%s.%s", (int)l, p, l && p[l - 1] == '.' ? "" : ".", name, leaf);
            uint8_t *b;
            if (roger_hostpath(riscos, host) && (b = slurp(host, n))) return b;
            p = e ? e + 1 : NULL;
        }
    }
    return NULL;
}

/* Font_FindFont: R1 name, R2/R3 size (1/16 pt), R4/R5 dpi (0: the mode's) */
static void find_font(void)
{
    char name[64];
    cstr(R[1], name, 1);
    const struct rg_mode *m = cur_mode();
    int xres = (int32_t)R[4] > 0 ? (int)R[4] : 180 >> m->xeig, yres = (int32_t)R[5] > 0 ? (int)R[5] : 180 >> m->yeig;
    ev("Font_FindFont", "sii", name, (int)strlen(name), R[2], R[3]);
    for (int h = 1; h <= MAXFONT; h++) {
        struct font *f = &fonts[h];
        if (f->uses && !strcasecmp(f->name, name) && f->xsize == (int)R[2] && f->ysize == (int)R[3] &&
            f->xres == xres && f->yres == yres) {
            f->uses++;
            R[0] = (uint32_t)(current = h); R[4] = (uint32_t)xres; R[5] = (uint32_t)yres;
            return;
        }
    }
    int h;
    for (h = 1; h <= MAXFONT && fonts[h].uses; h++) ;
    if (h > MAXFONT) { os_error(0x213, "Font cache full"); return; }
    long nm = 0, nb = 0;
    uint8_t *met = font_file(name, "IntMetrics", &nm), *bm = met ? font_file(name, "x90y45", &nb) : NULL;
    if (!met || !bm || nm < 52 + 256 || nm < 52 + 256 + 12 * met[48]) {
        free(met); free(bm);
        os_error(0x214, "Font not found");
        return;
    }
    struct font *f = &fonts[h];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->xsize = (int)R[2]; f->ysize = (int)R[3]; f->xres = xres; f->yres = yres; f->uses = 1;
    f->metrics = met; f->bitmaps = bm; f->nbitmaps = nb;
    /* the bitmap size to scale: the smallest at least as large (in pixels
     * per em, vertically) as wanted, or else the largest */
    double want = f->ysize / 16.0 * yres / 72, best = 0;
    int bk = -1;
    for (long p = 0; p + 16 <= nb && bm[p]; p += 16) {
        double ppem = bm[p] * (double)bm[p + 3] / 72;
        if (bm[p + 1] != 4 || rd32(bm + p + 8) + 0x214 > (uint32_t)nb) continue;
        if (bk < 0 || (best < want ? ppem > best : ppem >= want && ppem < best)) { bk = (int)p; best = ppem; }
    }
    if (bk < 0) { free(met); free(bm); f->uses = 0; os_error(0x214, "Font not found"); return; }
    f->block = rd32(bm + bk + 8);
    f->sx = (f->xsize / 16.0 * xres) / (bm[bk] * (double)bm[bk + 2]);
    f->sy = (f->ysize / 16.0 * yres) / (bm[bk] * (double)bm[bk + 3]);
    R[0] = (uint32_t)(current = h); R[4] = (uint32_t)xres; R[5] = (uint32_t)yres;
}

/* a character, scaled: each new pixel the average of the old ones it covers */
static const struct glyph *glyph(struct font *f, int c)
{
    struct glyph *g = &f->g[c];
    if (f->done[c]) return g;
    f->done[c] = 1;
    const uint8_t *tab = f->bitmaps + f->block + 0x14;
    uint32_t off = (uint32_t)(tab[2 * c] | tab[2 * c + 1] << 8);
    if (!off || f->block + 0x14 + off + 4 > (uint32_t)f->nbitmaps) return g;
    const uint8_t *s = tab + off;
    int sx0 = (int8_t)s[0], sy0 = (int8_t)s[1], sw = s[2], sh = s[3];
    if (!sw || !sh || f->block + 0x14 + off + 4 + (uint32_t)(sw * sh + 1) / 2 > (uint32_t)f->nbitmaps) return g;
    const uint8_t *px = s + 4;
    double kx = f->sx, ky = f->sy;
    g->x0 = (int)floor(sx0 * kx); g->y0 = (int)floor(sy0 * ky);
    g->w = (int)ceil((sx0 + sw) * kx) - g->x0; g->h = (int)ceil((sy0 + sh) * ky) - g->y0;
    if (g->w <= 0 || g->h <= 0) { g->w = g->h = 0; return g; }
    g->lv = calloc((size_t)g->w * g->h, 1);
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            /* the new pixel in the old ones' coordinates (from the bitmap's corner) */
            double ax = (g->x0 + x) / kx - sx0, bx = (g->x0 + x + 1) / kx - sx0;
            double ay = (g->y0 + y) / ky - sy0, by = (g->y0 + y + 1) / ky - sy0;
            double sum = 0, area = (bx - ax) * (by - ay);
            for (int j = (int)floor(ay); j < by; j++) {
                if (j < 0 || j >= sh) continue;
                double cy = fmin(by, j + 1) - fmax(ay, j);
                for (int i = (int)floor(ax); i < bx; i++) {
                    if (i < 0 || i >= sw) continue;
                    int k = j * sw + i, v = px[k / 2] >> (k & 1) * 4 & 15;
                    sum += v * cy * (fmin(bx, i + 1) - fmax(ax, i));
                }
            }
            int v = (int)lround(sum / area);
            g->lv[y * g->w + x] = (uint8_t)(v > 15 ? 15 : v);
        }
    return g;
}

/* a character's width: millipoints */
static double advance(struct font *f, int c)
{
    const uint8_t *m = f->metrics;
    int n = m[48], idx = m[52 + c];
    return idx < n ? rd16s(m + 52 + 256 + 8 * n + 2 * idx) * (double)f->xsize / 16 : 0;
}

/* a logical font colour's RGB (0xRRGGBB): the colour number as GCOL sets it */
static uint32_t logical_rgb(int c) { return vidc_rgb(0, vdu_colour_pixel(c)); }

/* the screen's closest pixel value to an RGB colour */
static uint8_t nearest(uint32_t rgb)
{
    int n = cur_mode()->bpp >= 8 ? 256 : 1 << cur_mode()->bpp, best = 0;
    long bd = -1;
    for (int p = 0; p < n; p++) {
        uint32_t q = vidc_rgb(0, (uint8_t)p);
        long dr = (long)(rgb >> 16 & 255) - (q >> 16 & 255), dg = (long)(rgb >> 8 & 255) - (q >> 8 & 255),
             db = (long)(rgb & 255) - (q & 255), d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (bd < 0 || d < bd) { bd = d; best = p; }
    }
    return (uint8_t)best;
}

/* Font_SetPalette's RGB (&BBGGRR00) -> 0xRRGGBB */
static uint32_t bbggrr(uint32_t v) { return (v >> 8 & 255) << 16 | (v >> 16 & 255) << 8 | (v >> 24 & 255); }

/* the pixel values of the 16 levels, for the current colours */
static void levels(uint8_t lvp[16])
{
    uint32_t b = pal[f_fg & 255].set ? pal[f_fg & 255].bg : logical_rgb(f_bg);
    uint32_t c = pal[f_fg & 255].set ? pal[f_fg & 255].fg : logical_rgb(f_fg);
    int steps = f_off < 1 ? 1 : f_off > 15 ? 15 : f_off;
    for (int v = 0; v < 16; v++) {
        int k = (v * steps + 7) / 15;                  /* the level's shade, 0..steps */
        uint32_t rgb = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            int x = (int)(b >> sh & 255), y = (int)(c >> sh & 255);
            rgb |= (uint32_t)(x + (y - x) * k / steps) << sh;
        }
        lvp[v] = nearest(rgb);
    }
}

/* Font_Paint: R1 the string, R2 flags (bit 4: R3/R4 in OS units, else
 * millipoints; bit 7: R7 its length; bit 8: R0 the font), R3, R4 where its
 * baseline starts. Control sequences: 9/11 move (3 bytes, millipoints), 17
 * a colour (byte: foreground), 18 colours (background, foreground, offset),
 * 19 RGB colours (bg r g b, fg r g b, offset), 21 a comment (to a control
 * character), 25 underline (2 bytes), 26 the font (a handle), 27/28 a
 * matrix (word-aligned: 4 or 6 words) */
static void paint(void)
{
    int h = (R[2] & 0x100) && R[0] ? (int)(R[0] & 255) : current;
    struct font *f = h >= 1 && h <= MAXFONT && fonts[h].uses ? &fonts[h] : NULL;
    if (!f) { os_error(0x210, "Undefined font handle"); return; }
    uint32_t s = R[1], lim = R[2] & 0x80 ? R[7] : ~0u;
    double x = (int32_t)R[3], y = (int32_t)R[4];        /* millipoints */
    if (R[2] & 0x10) { x *= 400; y *= 400; }
    char txt[256]; int tl = 0;
    for (uint32_t p = s; MEM[p] >= 32 && p - s < lim && tl < 255; p++) txt[tl++] = (char)MEM[p];
    txt[tl] = 0;
    ev("Font_Paint", "s", txt, tl);
    if (!rg_full_os) return;
    int ox, oy;
    vdu_origin(&ox, &oy);
    const struct rg_mode *m = cur_mode();
    uint8_t lvp[16];
    levels(lvp);
    for (uint32_t p = s; p - s < lim; p++) {
        uint8_t c = MEM[p];
        if (c == 9 || c == 11) {
            int d = MEM[p + 1] | MEM[p + 2] << 8 | (int8_t)MEM[p + 3] << 16;
            if (c == 9) x += d; else y += d;
            p += 3; continue;
        }
        if (c == 17) { f_fg = MEM[++p]; levels(lvp); continue; }
        if (c == 18) { f_bg = MEM[p + 1]; f_fg = MEM[p + 2]; f_off = (int8_t)MEM[p + 3]; p += 3; levels(lvp); continue; }
        if (c == 19) {
            f_fg = 255;
            pal[255].set = 1;
            pal[255].bg = (uint32_t)MEM[p + 1] << 16 | MEM[p + 2] << 8 | MEM[p + 3];
            pal[255].fg = (uint32_t)MEM[p + 4] << 16 | MEM[p + 5] << 8 | MEM[p + 6];
            f_off = (int8_t)MEM[p + 7]; p += 7; levels(lvp); continue;
        }
        if (c == 21) { while (MEM[p + 1] >= 32) p++; p++; continue; }
        if (c == 25) { p += 2; continue; }
        if (c == 26) {
            int k = MEM[++p];
            if (k >= 1 && k <= MAXFONT && fonts[k].uses) f = &fonts[current = k];
            continue;
        }
        if (c == 27 || c == 28) { p = ((p + 4) & ~3u) + (c == 27 ? 16 : 24) - 1; continue; }
        if (c < 32) break;
        const struct glyph *g = glyph(f, c);
        int bx = ((int)floor(x / 400) + ox) >> m->xeig, by = m->height - 1 - (((int)floor(y / 400) + oy) >> m->yeig);
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int v = g->lv[j * g->w + i];
                if (v) vdu_paint_pixel(bx + g->x0 + i, by - (g->y0 + j), lvp[v]);
            }
        x += advance(f, c);
    }
}

/* Font_StringWidth: R1 the string, R2/R3 the most (millipoints), R4 the
 * split character (-1 none), R5 the most characters -> R1 where it
 * stopped, R2/R3 the width, R4 split characters met, R5 characters */
static void string_width(void)
{
    struct font *f = current >= 1 && current <= MAXFONT && fonts[current].uses ? &fonts[current] : NULL;
    if (!f) { os_error(0x210, "Undefined font handle"); return; }
    double w = 0;
    uint32_t p = R[1], n = 0, splits = 0;
    while (MEM[p] >= 32 && n < R[5] && w + advance(f, MEM[p]) <= (int32_t)R[2]) {
        if ((int32_t)R[4] >= 0 && MEM[p] == R[4]) splits++;
        w += advance(f, MEM[p]); p++; n++;
    }
    R[1] = p; R[2] = (uint32_t)(int32_t)lround(w); R[3] = 0; R[4] = splits; R[5] = n;
}

int fonts_swi(uint32_t b)
{
    switch (b) {
    case 0x40081: find_font(); return 1;
    case 0x40082:                                     /* Font_LoseFont */
        if (R[0] >= 1 && R[0] <= MAXFONT && fonts[R[0]].uses && !--fonts[R[0]].uses) {
            struct font *f = &fonts[R[0]];
            for (int c = 0; c < 256; c++) free(f->g[c].lv);
            free(f->metrics); free(f->bitmaps);
            memset(f, 0, sizeof *f);
        }
        return 1;
    case 0x40085: string_width(); return 1;
    case 0x40086: paint(); return 1;
    case 0x40088: R[1] = (uint32_t)((int32_t)R[1] / 400); R[2] = (uint32_t)((int32_t)R[2] / 400); return 1;   /* Font_ConverttoOS */
    case 0x40089: R[1] *= 400; R[2] *= 400; return 1;                                                         /* Font_Converttopoints */
    case 0x4008A:                                     /* Font_SetFont */
        if (R[0] >= 1 && R[0] <= MAXFONT && fonts[R[0]].uses) current = (int)R[0];
        else os_error(0x210, "Undefined font handle");
        return 1;
    case 0x4008B: R[0] = (uint32_t)current; R[1] = (uint32_t)f_bg; R[2] = (uint32_t)f_fg; R[3] = (uint32_t)f_off; return 1;
    case 0x40092:                                     /* Font_SetFontColours: R0 font (0 current), R1 bg, R2 fg, R3 offset */
        if (R[0] >= 1 && R[0] <= MAXFONT && fonts[R[0]].uses) current = (int)R[0];
        f_bg = (int)R[1]; f_fg = (int)R[2]; f_off = (int32_t)R[3];
        return 1;
    case 0x40093:                                     /* Font_SetPalette: R1 bg, R2 fg, R3 offset, R4/R5 &BBGGRR00 */
        pal[R[2] & 255].set = 1;
        pal[R[2] & 255].bg = bbggrr(R[4]);
        pal[R[2] & 255].fg = bbggrr(R[5]);
        return 1;
    }
    return 0;
}

int fonts_swi_number(const char *s, uint32_t *n)
{
    static const struct { const char *name; uint32_t n; } names[] = {
        {"FindFont", 0x40081}, {"LoseFont", 0x40082}, {"StringWidth", 0x40085}, {"Paint", 0x40086},
        {"ConverttoOS", 0x40088}, {"Converttopoints", 0x40089}, {"SetFont", 0x4008A},
        {"CurrentFont", 0x4008B}, {"SetFontColours", 0x40092}, {"SetPalette", 0x40093},
    };
    if (strncmp(s, "Font_", 5)) return 0;
    for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++)
        if (!strcmp(s + 5, names[k].name)) { *n = names[k].n; return 1; }
    return 0;
}
