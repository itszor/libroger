/* ColourTrans: colours as &BBGGRR00 palette entries, matched to the
 * nearest a mode can show.
 *
 * A palette argument is a pointer to one word per colour (&BBGGRR00), -1
 * the current palette (the default one for a mode other than the current)
 * or 0 the mode's default palette; a mode -1 is the current one. Matching
 * is by the smallest squared distance in RGB. The SWIs (&40740 on), as
 * the PRMs give them:
 *   &40740 SelectTable, &40763 GenerateTable (R0/R1 source mode, palette;
 *     R2/R3 destination mode, palette; R4 the table, one byte a source
 *     colour, or 0: R4 its size), up to 256 colours each way
 *   &40742 ReturnGCOL, &40744 ReturnColourNumber (R0 a colour), and
 *     &40745/&40746 for a mode (R1) and palette (R2)
 *   &40743 SetGCOL (R0 the colour, R3 bit 7 background, R4 the action),
 *     &40761 SetTextColour (R0, R3 bit 7 background): the nearest colour
 *     made the current one with the VDU drivers
 *   &4075C ReadPalette (R0/R1 mode, palette; R2 a buffer or 0, R3 its
 *     size -> the size needed, or what's left)
 *   &4075D WritePalette (R2 a palette, or 0 the default; R4 bit 0: the
 *     pointer's colours, not kept): the current mode's colours set
 *   &40750 InvalidateCache: nothing kept */
#include <string.h>
#include "internal.h"

static int bpp_of(uint32_t mode)
{
    const struct rg_mode *m = (int32_t)mode < 0 ? cur_mode() : mode_info(mode);
    return m ? m->bpp : cur_mode()->bpp;
}

static int ncolours(uint32_t mode)
{
    int b = bpp_of(mode);
    return b > 8 ? 256 : 1 << b;
}

/* colour k of a mode's palette (pal: as above), 0xRRGGBB */
static uint32_t colour_of(uint32_t mode, uint32_t pal, int k)
{
    if (pal != 0 && pal != 0xFFFFFFFFu) {
        uint32_t v = RD32(pal + 4 * k);
        return (v >> 8 & 0xFF) << 16 | (v >> 16 & 0xFF) << 8 | v >> 24;
    }
    if (pal == 0xFFFFFFFFu && ((int32_t)mode < 0 || bpp_of(mode) == cur_mode()->bpp)) return video_colour((uint8_t)k);
    return video_default_colour(bpp_of(mode), (uint8_t)k);
}

static int nearest(uint32_t bbggrr00, uint32_t mode, uint32_t pal)
{
    int r = bbggrr00 >> 8 & 0xFF, g = bbggrr00 >> 16 & 0xFF, b = bbggrr00 >> 24, best = 0;
    long bd = -1;
    for (int k = 0; k < ncolours(mode); k++) {
        uint32_t c = colour_of(mode, pal, k);
        long dr = (long)(c >> 16) - r, dg = (long)(c >> 8 & 0xFF) - g, db = (long)(c & 0xFF) - b;
        long d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd) { bd = d; best = k; }
    }
    return best;
}

/* a colour number's GCOL number: in 256-colour modes bits 2-7 (the
 * rest is the tint) */
static uint32_t gcol_of(int c, uint32_t mode) { return bpp_of(mode) == 8 ? (uint32_t)c >> 2 : (uint32_t)c; }

static void set_colour(int c, int background, int action, int text)
{
    int eight = cur_mode()->bpp == 8;
    uint32_t g = gcol_of(c, (uint32_t)-1);
    if (text) { vdu_put(17); vdu_put((uint8_t)(g | (background ? 128 : 0))); }
    else { vdu_put(18); vdu_put((uint8_t)action); vdu_put((uint8_t)(g | (background ? 128 : 0))); }
    if (eight) {                                  /* the tint: VDU 23,17 */
        uint8_t v[10] = {23, 17, (uint8_t)((text ? 0 : 2) + (background ? 1 : 0)), (uint8_t)((c & 3) << 6)};
        for (int k = 0; k < 10; k++) vdu_put(v[k]);
    }
}

static void table(void)
{
    uint32_t sm = R[0], sp = R[1], dm = R[2], dp = R[3];
    int n = ncolours(sm);
    if (!R[4]) { R[4] = (uint32_t)n; return; }
    for (int k = 0; k < n; k++) {
        uint32_t c = colour_of(sm, sp, k);
        MEM[R[4] + k] = (uint8_t)nearest((c & 0xFF) << 24 | (c >> 8 & 0xFF) << 16 | (c >> 16) << 8, dm, dp);
    }
}

int colourtrans_swi(uint32_t b)
{
    if (b < 0x40740 || b > 0x4077F) return 0;
    switch (b) {
    case 0x40740: case 0x40763: table(); return 1;               /* SelectTable, GenerateTable */
    case 0x40742: R[0] = gcol_of(nearest(R[0], (uint32_t)-1, (uint32_t)-1), (uint32_t)-1); return 1;
    case 0x40744: R[0] = (uint32_t)nearest(R[0], (uint32_t)-1, (uint32_t)-1); return 1;
    case 0x40745: R[0] = gcol_of(nearest(R[0], R[1], R[2]), R[1]); return 1;   /* ...ForMode */
    case 0x40746: R[0] = (uint32_t)nearest(R[0], R[1], R[2]); return 1;
    case 0x40743: {                                               /* SetGCOL */
        int c = nearest(R[0], (uint32_t)-1, (uint32_t)-1);
        set_colour(c, R[3] & 0x80, (int)(R[4] & 0xFF), 0);
        R[0] = gcol_of(c, (uint32_t)-1); R[2] = (uint32_t)cur_mode()->bpp;
        return 1;
    }
    case 0x40761: {                                               /* SetTextColour */
        int c = nearest(R[0], (uint32_t)-1, (uint32_t)-1);
        set_colour(c, R[3] & 0x80, 0, 1);
        R[0] = gcol_of(c, (uint32_t)-1);
        return 1;
    }
    case 0x4075C: {                                               /* ReadPalette */
        uint32_t n = (uint32_t)ncolours(R[0]);
        if (!R[2]) { R[3] = 4 * n; return 1; }
        uint32_t k = 0;
        for (; k < n && 4 * (k + 1) <= R[3]; k++) {
            uint32_t c = colour_of(R[0], R[1], (int)k);
            WR32(R[2] + 4 * k, (c & 0xFF) << 24 | (c >> 8 & 0xFF) << 16 | (c >> 16) << 8);
        }
        R[3] -= 4 * k;
        return 1;
    }
    case 0x4075D: {                                               /* WritePalette: R2 the palette */
        if (R[4] & 1) return 1;                                   /* (bit 0: the pointer's: not kept) */
        int n = ncolours((uint32_t)-1);
        for (int k = 0; k < n; k++)
            video_set_colour(k, R[2] ? colour_of(0, R[2], k) : video_default_colour(cur_mode()->bpp, (uint8_t)k));
        return 1;
    }
    case 0x40750: return 1;                                       /* InvalidateCache */
    }
    char name[48];
    snprintf(name, sizeof name, "ColourTrans_%x", b);
    ev(name, "iii", R[0], R[1], R[2]);
    os_error(0x1E6, "ColourTrans SWI not supported");
    return 1;
}
