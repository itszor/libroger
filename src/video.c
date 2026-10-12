/* Video: screen modes and memory, the VDU stream, MEMC's video DMA
 * registers, VIDC (palette and vertical timing), and composing the frame
 * the display hardware would show.
 *
 * Screen memory ends at &2000000 by default, as RISC OS maps it, and is
 * physical address 0 for MEMC's DMA. A program uses it through the OS
 * (screen banks: OS_Byte 112/113, OS_Word 22) or by programming MEMC's
 * Vinit/Vstart/Vend itself (e.g. a scrolling play area flipped between two
 * buffers above a fixed panel reached when DMA wraps from Vend to Vstart);
 * the latter takes over once it happens. VIDC's VDSR/VDER crop the picture
 * vertically, and its HDSR/HDER and HBSR/HBER set where each line starts,
 * how long it is and where the border hides it; the output window is
 * always the standard mode's (256 lines of its width). Modes
 * that only a Risc PC's VIDC20 has (taller, or 16/32 bits per pixel) are
 * shown as they are, with the gamma tables PaletteV sets. */
#include <string.h>
#include "internal.h"

/* ---------------------------------------------------------------- modes
 * The standard graphics modes of the Archimedes' 50Hz monitors, and with
 * roger_config.vidc20 (a Risc PC) the multisync ones too, and RISC OS
 * 3.5's mode selectors: any size, 1 to 32 bits per pixel; and modes
 * modules define (Service_ModeExtension, below). A mode is
 * identified as RISC OS does: a number below &100, else the address of a
 * selector block (+0 flags 1, +4 x pixels, +8 y pixels, +12 log2 bpp, +16
 * frame rate, then mode variable / value pairs up to -1). */
static const struct rg_mode modes[] = {
    /*  w    h  bpp xe ye cols rows */
    [0]  = {640, 256, 1, 1, 2, 80, 32},
    [1]  = {320, 256, 2, 2, 2, 40, 32},
    [2]  = {160, 256, 4, 3, 2, 20, 32},
    [4]  = {320, 256, 1, 2, 2, 40, 32},
    [5]  = {160, 256, 2, 3, 2, 20, 32},
    [8]  = {640, 256, 2, 1, 2, 80, 32},
    [9]  = {320, 256, 4, 2, 2, 40, 32},
    [10] = {160, 256, 8, 3, 2, 20, 32},
    [12] = {640, 256, 4, 1, 2, 80, 32},
    [13] = {320, 256, 8, 2, 2, 40, 32},
    [15] = {640, 256, 8, 1, 2, 80, 32},
    /* multisync / VGA (VIDC20 only) */
    [18] = {640, 512, 1, 1, 1, 80, 64},
    [19] = {640, 512, 2, 1, 1, 80, 64},
    [20] = {640, 512, 4, 1, 1, 80, 64},
    [21] = {640, 512, 8, 1, 1, 80, 64},
    [25] = {640, 480, 1, 1, 1, 80, 60},
    [26] = {640, 480, 2, 1, 1, 80, 60},
    [27] = {640, 480, 4, 1, 1, 80, 60},
    [28] = {640, 480, 8, 1, 1, 80, 60},
};
uint32_t screen_mode = 12;                /* a mode number, or the selector's address (MODE_SEL) */
uint32_t bank, display_bank;              /* OS_Byte 112 (VDU drivers) / 113 (display), 0-based */
static uint32_t display_off;              /* OS_Word 22: display start offset */
static struct rg_mode current = {640, 256, 4, 1, 2, 80, 32};

/* The 50Hz Archimedes modes (256 lines, up to 8bpp): MEMC and VIDC1's
 * raster timing apply. */
static int vidc1_mode(const struct rg_mode *m) { return m->height == 256 && m->bpp <= 8; }

/* A mode a module defines: Service_ModeExtension (&50, R2 the mode, R3 the
 * monitor type - 0, a TV-standard monitor) answered with R3 a VIDC list
 * (+0 format, +4 base mode, then VIDC register values up to -1) and R4 a
 * workspace list (+0 flags, +4 base mode, then mode variable / value pairs
 * up to -1). The geometry comes from the workspace list, what it leaves
 * out from the base mode (or, for the depth, VIDC's control register);
 * such a mode is shown as it is, like a Risc PC's (no MEMC/VIDC1 raster
 * effects). Defined modes are remembered; a mode nobody claims is asked
 * about again next time (FervourMode claims the number it found free only
 * once *FModeOn has chosen it). */
static struct rg_mode ext_modes[128];
static uint8_t ext_known[128];

static const struct rg_mode *module_mode(uint32_t m)
{
    static int asking;
    if (ext_known[m]) return &ext_modes[m];
    if (asking) return NULL;                       /* a module's own mode checks */
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    R[1] = 0x50; R[2] = m; R[3] = 0; R[4] = 0;
    asking = 1;
    int claimed = modules_service();
    asking = 0;
    uint32_t vl = R[3], wl = R[4];
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    if (!claimed || !wl || wl + 8 > MEMSIZE) return NULL;
    uint32_t base_mode = RD32(wl + 4) & 0x7F;
    struct rg_mode r = base_mode < sizeof modes / sizeof modes[0] && modes[base_mode].width
                       ? modes[base_mode] : (struct rg_mode){640, 256, 8, 1, 2, 80, 32};
    if (vl && vl + 8 <= MEMSIZE)
        for (uint32_t p = vl + 8; p < vl + 8 + 4 * 64 && RD32(p) != 0xFFFFFFFF; p += 4)
            if (RD32(p) >> 26 == 0x38) r.bpp = 1 << (RD32(p) >> 2 & 3);   /* control register */
    int32_t v[13];
    for (int k = 0; k < 13; k++) v[k] = -1;
    for (uint32_t p = wl + 8; p < wl + 8 + 8 * 64 && RD32(p) != 0xFFFFFFFF; p += 8)
        if (RD32(p) < 13) v[RD32(p)] = (int32_t)RD32(p + 4);
    if (v[9] >= 0) r.bpp = 1 << v[9];
    if (v[4] >= 0) r.xeig = v[4];
    if (v[5] >= 0) r.yeig = v[5];
    if (v[6] > 0) r.width = v[6] * 8 / r.bpp; else if (v[11] >= 0) r.width = v[11] + 1;
    if (v[12] >= 0) r.height = v[12] + 1; else if (v[7] > 0 && v[6] > 0) r.height = v[7] / v[6];
    r.text_cols = v[1] >= 0 ? v[1] + 1 : r.width / 8;
    r.text_rows = v[2] >= 0 ? v[2] + 1 : r.height / 8;
    if (r.width < 16 || r.height < 16 || r.width > 2048 || r.height > 2048) return NULL;
    /* A mode a module defines keeps the pixel shape its eig factors give it
     * against the 1280x1024 OS units that fill the 4:3 screen: a smaller or
     * larger picture rather than one stretched to fill (FervourMode's
     * 368x216, mode 13's pixels, is wider than 4:3). */
    int osw = r.width << r.xeig, osh = r.height << r.yeig;
    r.aspect = osw == 1280 && osh == 1024 ? 0 : 4.0f / 3 * osw * 1024 / (1280.0f * osh);
    ext_modes[m] = r;
    ext_known[m] = 1;
    ev("mode_extension", "iiii", m, r.width, r.height, r.bpp);
    return &ext_modes[m];
}

/* m's geometry, or NULL if it isn't a mode this machine has */
const struct rg_mode *mode_info(uint32_t m)
{
    static struct rg_mode sel;
    if (m < 0x100) {
        m &= 0x7F;
        if (m >= sizeof modes / sizeof modes[0] || !modes[m].width) return module_mode(m);
        if (!vidc1_mode(&modes[m]) && !rg->vidc20) return module_mode(m);
        return &modes[m];
    }
    if (!rg->vidc20 || m + 20 > MEMSIZE || (RD32(m) & 0xFF) != 1) return NULL;
    uint32_t w = RD32(m + 4), h = RD32(m + 8), l2 = RD32(m + 12);
    if (w < 16 || h < 16 || w > 2048 || h > 2048 || l2 > 5) return NULL;
    sel = (struct rg_mode){(int)w, (int)h, 1 << l2, 1, h * 2 < w ? 2 : 1, (int)w / 8, (int)h / 8};
    for (uint32_t p = m + 20; p + 8 <= MEMSIZE && RD32(p) != 0xFFFFFFFF && p < m + 20 + 8 * 32; p += 8) {
        if (RD32(p) == 4) sel.xeig = RD32(p + 4);
        if (RD32(p) == 5) sel.yeig = RD32(p + 4);
    }
    if (mode_bytes(&sel) > 0x200000) return NULL;          /* more than the Risc PC's 2MB of VRAM */
    return &sel;
}
const struct rg_mode *cur_mode(void) { return &current; }
float mode_aspect(const struct rg_mode *m) { return m->aspect > 0 ? m->aspect : 4.0f / 3; }

/* Change mode (VDU 22, OS_ScreenMode 0, Wimp_SetMode): 0 if m isn't one.
 * A selector is copied (OS_ScreenMode 1 returns the copy). */
static void reset_timing(void);
int video_set_mode(uint32_t m)
{
    const struct rg_mode *mi = mode_info(m);
    if (!mi) return 0;
    current = *mi;
    if (m >= 0x100) {
        uint32_t len = 20;
        while (len < 0x100 - 8 && RD32(m + len) != 0xFFFFFFFF) len += 8;
        if (m != MODE_SEL) memmove(MEM + MODE_SEL, MEM + m, len + 4);
        m = MODE_SEL;
    }
    screen_mode = m;
    bank = display_bank = 0;
    pointer_shape = 0;
    video_default_palette();
    reset_timing();                       /* the OS programs VIDC and MEMC for the mode */
    mbox[0] = mbox[1] = 0;                /* the mouse's bounding box: the whole screen again */
    mbox[2] = (current.width << current.xeig) - 1;
    mbox[3] = (current.height << current.yeig) - 1;
    if (rg_full_os) vdu_mode_reset();
    if (rg_desktop) desktop_mode_changed();
    return 1;
}
uint32_t mode_bytes(const struct rg_mode *m) { return (uint32_t)m->width * m->height * m->bpp / 8; }
static uint32_t screen_bytes;             /* after OS_ChangeDynamicArea */
uint32_t screen_size(void) { return screen_bytes ? screen_bytes : rg->screen_size ? rg->screen_size : 0x14000; }
void video_set_screen_size(uint32_t size) { screen_bytes = size; }
uint32_t screen_base(void) { return rg->screen_base ? rg->screen_base : 0x2000000u - screen_size(); }
uint32_t write_bank_addr(void) { return screen_base() + bank * mode_bytes(cur_mode()); }
/* OS_Byte 112/113. The display bank and OS_Word 22's display start set the
 * same thing (VIDC's display start): the last one counts. */
void video_set_bank(int which, uint32_t b) { if (which) { display_bank = b; display_off = 0; } else bank = b; }

/* VDU variables (OS_ReadVduVariables) and mode variables (OS_ReadModeVariable) */
static uint32_t log2bpp(int bpp) { return bpp == 32 ? 5 : bpp == 16 ? 4 : bpp == 8 ? 3 : bpp == 4 ? 2 : bpp == 2 ? 1 : 0; }

uint32_t modevar(int64_t mode, uint32_t v)
{
    const struct rg_mode *m = mode < 0 ? cur_mode() : mode_info((uint32_t)mode);
    if (!m) return 0;
    switch (v) {
    case 0: return 0;                          /* ModeFlags */
    case 1: return m->text_cols - 1;           /* ScrRCol */
    case 2: return m->text_rows - 1;           /* ScrBRow */
    case 3: return m->bpp == 32 ? 0xFFFFFFFF : m->bpp == 16 ? 0xFFFF   /* NColour (63 in */
                 : m->bpp == 8 ? 63 : (1u << m->bpp) - 1;                  /* 256-colour modes) */
    case 4: return m->xeig;
    case 5: return m->yeig;
    case 6: return (uint32_t)m->width * m->bpp / 8;          /* LineLength */
    case 7: return mode_bytes(m);              /* ScreenSize */
    case 8: return 0;                          /* YShftFactor */
    case 9: case 10: return log2bpp(m->bpp);  /* Log2BPP, Log2BPC */
    case 11: return m->width - 1;              /* XWindLimit */
    case 12: return m->height - 1;             /* YWindLimit */
    default: return 0;
    }
}

uint32_t vduvar(uint32_t v)
{
    uint32_t sz = mode_bytes(cur_mode());
    switch (v) {
    case 148: return screen_base() + bank * sz;                       /* DriverBank's address */
    case 149: return screen_base() + (rg_full_os ? display_bank : bank) * sz;   /* DisplayBank's */
    case 150: return screen_size();                                   /* TotalScreenSize */
    default: return v < 13 ? modevar(-1, v) : 0;
    }
}

/* the standard 16 colours (0x0BGR) */
static const uint32_t std16[16] = {
    0x000, 0x00F, 0x0F0, 0x0FF, 0xF00, 0xF0F, 0xFF0, 0xFFF,
    0x555, 0x009, 0x090, 0x099, 0x900, 0x909, 0x990, 0xAAA};

/* A Risc PC's 256-colour modes: VIDC20 has a palette entry, 8 bits a gun,
 * for each of the 256 pixel values, and VDU 19 / OS_Word 11 and 12 address
 * them all (RISC OS 3.5). An Archimedes' VIDC1 has 16 entries, and the top
 * four bits of a pixel drive the colour directly (vidc_rgb()). */
static uint32_t vidc20_pal[256];                  /* 0xRRGGBB */
static uint32_t vidc1_rgb(const uint32_t *pal, uint8_t p);
static int full_palette(void) { return rg->vidc20 && cur_mode()->bpp == 8; }
static uint32_t rgb12(uint32_t v) { return (v & 0xF) * 17 << 16 | (v >> 4 & 0xF) * 17 << 8 | (v >> 8 & 0xF) * 17; }

/* ---------------------------------------------------------------- VDU stream
 * Every byte written is recorded (finish() saves it); interactive runs also
 * pass it to the VDU drivers (vdu.c). Mode changes are followed here. */
static uint8_t vdu_log[1 << 20];
static size_t nvdu;
void video_save_vdu(FILE *f) { fwrite(vdu_log, 1, nvdu, f); }

void vdu_put(uint8_t c)
{
    static const uint8_t nparams[32] = {0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                        0,1,2,5,0,0,1,9,8,5,0,0,4,4,0,2};
    static uint8_t q[10]; static int qn, qwant, disabled;
    if (nvdu < sizeof vdu_log) vdu_log[nvdu++] = c;
    /* VDU 21 disables the VDU drivers until VDU 6 (sequences are still
     * parsed, so that a 6 among a sequence's parameters doesn't count) */
    if (!qwant && (c == 6 || c == 21)) { disabled = c == 21; return; }
    if (rg_full_os && !disabled) vdu_write(c);
    if (qwant) {
        q[qn++] = c;
        if (qn == qwant) {
            if (q[0] == 22 && !disabled) video_set_mode(q[1]);
            if (q[0] == 19 && !disabled && full_palette()) {
                if (q[2] == 16) vidc20_pal[q[1]] = (uint32_t)q[3] << 16 | (uint32_t)q[4] << 8 | q[5];
                else if (q[2] < 16) vidc20_pal[q[1]] = rgb12(std16[q[2]]);
            } else if (q[0] == 19 && !disabled) {   /* palette: logical, physical (16: R,G,B) */
                int l = q[1] & 15, p = q[2];
                if (p == 16) vidc_palette[l] = (q[3] >> 4) | (uint32_t)(q[4] >> 4) << 4 | (uint32_t)(q[5] >> 4) << 8;
                else if (p < 16) vidc_palette[l] = std16[p];    /* (24 border, 25 pointer: not shown) */
            }
            qn = qwant = 0;
        }
    } else if (c < 32 && nparams[c]) {
        q[0] = c; qn = 1; qwant = 1 + nparams[c];
    }
}

/* ---------------------------------------------------------------- VIDC
 * Writes to &3400000-&35FFFFF: value | register << 26 (the address is
 * ignored). Registers 0-15 are the palette (4 bits each of R, G, B, bits
 * 0-11), 16 the border; 40 VCR, 43 VDSR and 44 VDER give the vertical
 * timing, in raster lines from the start of vertical sync (bits 14-23),
 * and 42 VBSR and 45 VBER the border, covering lines of the display above
 * VBSR and from VBER (a program hiding a few lines it draws into);
 * 34 HBSR, 35 HDSR, 36 HDER and 37 HBER the horizontal, in units of two
 * pixels from the start of horizontal sync (bits 14-23). Until the program
 * writes them, the standard 50Hz modes' values apply. */
uint32_t vidc_palette[16];               /* 0x0BGR per entry */
uint32_t vidc_palette_top[16];           /* lines above palette_split */
int palette_split;
#define STD_VDSR 38
static int vidc_vcr = 312, vidc_vdsr = STD_VDSR, vidc_vder = STD_VDSR + 256;
static int vidc_hdsr = -1, vidc_hder = -1, vidc_hbsr = -1, vidc_hber = -1;   /* -1: the mode's own */
static int vidc_vbsr = -1, vidc_vber = -1;
static int vidc_seen[64];
static int memc_direct;
static uint32_t memc_vinit, memc_vstart = 0, memc_vend = ~0u;

/* a mode change: the mode's standard vertical timing, and the display
 * starting at the screen's start (a program's own VIDC/MEMC settings, and
 * an OS_Word 22 display start, are undone) */
static void reset_timing(void)
{
    vidc_vcr = 312; vidc_vdsr = STD_VDSR; vidc_vder = STD_VDSR + 256;
    vidc_hdsr = vidc_hder = vidc_hbsr = vidc_hber = -1;
    vidc_vbsr = vidc_vber = -1;
    memc_direct = 0; memc_vinit = 0; memc_vstart = 0; memc_vend = ~0u;
    display_off = 0;
}

void vidc_write(uint32_t v)
{
    unsigned reg = v >> 26;
    uint32_t val = v & 0x3FFFFFF;
    if (reg < 16) vidc_palette[reg] = val;
    if (reg == 40) vidc_vcr = val >> 14;
    if (reg == 43) vidc_vdsr = val >> 14;
    if (reg == 44) vidc_vder = val >> 14;
    if (reg == 34) vidc_hbsr = val >> 14;
    if (reg == 35) vidc_hdsr = val >> 14;
    if (reg == 36) vidc_hder = val >> 14;
    if (reg == 37) vidc_hber = val >> 14;
    if (reg == 42) vidc_vbsr = val >> 14;
    if (reg == 45) vidc_vber = val >> 14;
    if (val != 0 || vidc_seen[reg] < 8) {            /* vidc.log: the first few of each, and non-zero */
        vidc_seen[reg]++;
        FILE *f = fopen("vidc.log", "a");
        if (f) { fprintf(f, "%llu: VIDC reg %u = %#x (raw %#x)\n",
                          (unsigned long long)icount, reg, val, v); fclose(f); }
    }
}

/* "Vsync" is VIDC's flyback signal, raised at the end of the display
 * (VDER); a timer started there with this latch (2MHz, 128 ticks a line)
 * fires at raster line VDER + latch/128 of the next frame. Returns that as
 * a line of the output window (with standard timings: latch/128 - 56). */
int video_timer_line(uint32_t latch)
{
    int line = vidc_vder + (int)((latch + 1) / 128) - vidc_vcr - STD_VDSR;
    return line < 0 ? 0 : line > 256 ? 256 : line;
}

/* Host colour (0xRRGGBB) of a pixel value on an output line. With 8 bits
 * per pixel, bits 0-3 go through the palette and bits 4-7 drive the top
 * bits of the colour directly: bit 4 red 3, bit 5 green 2, bit 6 green 3,
 * bit 7 blue 3. */
uint32_t vidc_rgb(int line, uint8_t p)
{
    if (full_palette()) return vidc20_pal[p];
    return vidc1_rgb(line < palette_split ? vidc_palette_top : vidc_palette, p);
}

static uint32_t vidc1_rgb(const uint32_t *pal, uint8_t p)
{
    uint32_t v = pal[p & 15];
    int r = v & 0xF, g = (v >> 4) & 0xF, b = (v >> 8) & 0xF;
    if (cur_mode()->bpp == 8) {
        r = (r & 7) | (p >> 4 & 1) << 3;
        g = (g & 3) | (p >> 5 & 1) << 2 | (p >> 6 & 1) << 3;
        b = (b & 7) | (p >> 7 & 1) << 3;
    }
    return (uint32_t)(r * 17) << 16 | (uint32_t)(g * 17) << 8 | (uint32_t)(b * 17);
}

/* The palette a mode change sets up: the standard 16 colours, or for
 * 256-colour modes the 16 entries the default 256-colour palette needs
 * (bits 0-1 tint, bit 2 red 2, bit 3 blue 2). */
void video_default_palette(void)
{
    for (int c = 0; c < 16; c++) {
        if (cur_mode()->bpp == 8) {
            int t = c & 3;
            vidc_palette[c] = (uint32_t)((c >> 2 & 1) << 2 | t) | (uint32_t)t << 4 | (uint32_t)((c >> 3 & 1) << 2 | t) << 8;
        } else vidc_palette[c] = std16[c];
    }
    if (full_palette())                       /* the same colours, in all 256 entries */
        for (int p = 0; p < 256; p++) vidc20_pal[p] = vidc1_rgb(vidc_palette, (uint8_t)p);
}

/* ---------------------------------------------------------------- MEMC
 * Writes to &3600000-&37FFFFF: the register is address bits 17-19, the
 * value address bits 2-16, a physical address in 16-byte units (the data
 * is ignored). DMA starts each frame at Vinit and, after the 16 bytes at
 * Vend, continues at Vstart. The sound DMA registers are sound.c's.
 * (memc_vinit etc. are defined above, with reset_timing.) */
void memc_write(uint32_t a)
{
    uint32_t v = ((a >> 2) & 0x7FFF) << 4;
    switch ((a >> 17) & 7) {
    case 0: memc_vinit = v; memc_direct = 1; break;
    case 1: memc_vstart = v; break;
    case 2: memc_vend = v; break;
    case 4: case 5: case 6: sound_memc((a >> 17) & 7, v); break;   /* Sstart, SendN, Sptr */
    default: break;                     /* Cinit, control register */
    }
}

/* Horizontally, output pixel x of the window is raster pixel start + x
 * (pixels from the start of sync): where a standard mode's display starts,
 * as RISC OS's mode tables set HDSR - 131 at 8MHz (320 pixels wide), 261
 * at 16MHz (640). VIDC's display starts at HDSR * 2 plus a delay that
 * depends on the depth (19, 11, 7, 5 pixels at 1, 2, 4, 8 bpp) and is
 * (HDER - HDSR) * 2 pixels long; the border covers raster pixels before
 * HBSR * 2 + 1 and from HBER * 2 + 1. */
static int hpix(const uint8_t *line, int i, int bpp)
{
    int per = 8 / bpp;
    return line[i / per] >> (i % per * bpp) & ((1 << bpp) - 1);
}

static void put_hpix(uint8_t *line, int i, int bpp, int v)
{
    int per = 8 / bpp, sh = i % per * bpp;
    line[i / per] = (uint8_t)((line[i / per] & ~(((1 << bpp) - 1) << sh)) | v << sh);
}

/* The frame as the display hardware shows it: 256 lines of the current
 * mode's pixels (output lines outside VDSR..VDER or VBSR..VBER, and
 * pixels outside the display or inside the border horizontally, are
 * border, shown as colour 0). */
const uint8_t *displayed(void)
{
    static uint8_t frame[640 * 256], line[2048];
    const struct rg_mode *m = cur_mode();
    uint32_t sz = mode_bytes(m), row = sz / 256, total = screen_size();
    int hprog = (m->width == 320 || m->width == 640) &&
                ((vidc_hdsr >= 0 && vidc_hder > vidc_hdsr) || vidc_hbsr >= 0 || vidc_hber >= 0);
    int vb = vidc_vbsr > vidc_vdsr || (vidc_vber >= 0 && vidc_vber < vidc_vder);   /* border over the display */
    if (!vidc1_mode(m) || (!memc_direct && !hprog && !vb && vidc_vdsr == STD_VDSR && vidc_vder == STD_VDSR + 256))
        return MEM + screen_base() + display_off + display_bank * sz;
    uint32_t p = memc_direct ? memc_vinit : display_off + display_bank * sz;
    uint32_t end = memc_direct && memc_vend != ~0u ? memc_vend + 16 : total;
    int first = vidc_vdsr - STD_VDSR, last = vidc_vder - STD_VDSR;   /* output lines shown */
    static const int delay[9] = {0, 19, 11, 0, 7, 0, 0, 0, 5};
    int bpp = m->bpp, start = m->width == 320 ? 131 : 261;
    int dstart = start, dpx = m->width;                            /* the display, in raster pixels */
    if (hprog && vidc_hdsr >= 0 && vidc_hder > vidc_hdsr) {
        dstart = vidc_hdsr * 2 + delay[bpp];
        dpx = (vidc_hder - vidc_hdsr) * 2;
    }
    int bl = vidc_hbsr >= 0 ? vidc_hbsr * 2 + 1 : 0, br = vidc_hber >= 0 ? vidc_hber * 2 + 1 : 1 << 30;
    uint32_t len = (uint32_t)dpx * bpp / 8;                        /* bytes of DMA a line */
    if (len > sizeof line) len = sizeof line;
    int plain = dstart == start && len == row && bl <= start && br >= start + m->width;
    for (int y = 0; y < 256; y++) {
        uint8_t *d = frame + y * row;
        if (y < first || y >= last) { memset(d, 0, row); continue; }
        uint8_t *l = plain ? d : line;
        for (uint32_t n = 0; n < len; n++) {
            l[n] = MEM[screen_base() + (p % total)];
            if (++p == end) p = memc_direct ? memc_vstart : 0;
        }
        int ry = y + STD_VDSR;                                     /* its raster line */
        if (vb && ((vidc_vbsr >= 0 && ry < vidc_vbsr) || (vidc_vber >= 0 && ry >= vidc_vber))) {
            memset(d, 0, row);                                     /* under the border */
            continue;
        }
        if (plain) continue;
        for (int x = 0; x < m->width; x++) {
            int r = start + x, i = r - dstart;
            put_hpix(d, x, bpp, r < bl || r >= br || i < 0 || i >= dpx ? 0 : hpix(line, i, bpp));
        }
    }
    return frame;
}

/* ---------------------------------------------------------------- PaletteV
 * Gamma correction (VIDC20): reason 9 sets the red, green and blue tables
 * (R0-R2, 256 bytes each) every output colour goes through. The OS's own
 * PaletteV handler, for OS_CallAVector with nobody claiming the vector. */
uint8_t gamma_tab[3][256];
int gamma_set;
int video_palettev(void)
{
    if (R[4] != 9 || !rg->vidc20) return 0;
    for (int k = 0; k < 3; k++) memcpy(gamma_tab[k], MEM + R[k], 256);
    gamma_set = 1;
    ev("PaletteV", "i", 9);
    R[4] = 0;                                     /* handled */
    return 1;
}

/* ---------------------------------------------------------------- OS_Word */
int pointer_shape;                              /* a mode change turns the pointer off */
int mbox[4] = {0, 0, 1279, 1023};               /* OS_Word 21,1: left, bottom, right, top
                                                   (a mode change resets it to the screen) */
static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int video_os_word(uint32_t r0, uint8_t *blk)
{
    if (r0 == 21 && (blk[0] == 4 || blk[0] == 6)) {     /* read the mouse (unbuffered) or pointer */
        int x = clamp_i(mouse_x, mbox[0], mbox[2]), y = clamp_i(mouse_y, mbox[1], mbox[3]);   /* position */
        blk[1] = (uint8_t)x; blk[2] = (uint8_t)(x >> 8); blk[3] = (uint8_t)y; blk[4] = (uint8_t)(y >> 8);
    }
    if (r0 == 21 && (blk[0] == 3 || blk[0] == 5)) {    /* set mouse / pointer position */
        int x = clamp_i((int16_t)(blk[1] | blk[2] << 8), mbox[0], mbox[2]);
        int y = clamp_i((int16_t)(blk[3] | blk[4] << 8), mbox[1], mbox[3]);
        if (rg_interactive) display_set_mouse(x, y);
        else { mouse_x = x; mouse_y = y; }
    }
    if (r0 == 21 && blk[0] == 1)                        /* define the pointer's bounding box */
        for (int k = 0; k < 4; k++) mbox[k] = (int16_t)(blk[1 + 2 * k] | blk[2 + 2 * k] << 8);
    if ((r0 == 11 || r0 == 12) && full_palette()) {
        uint32_t *e = &vidc20_pal[blk[0]];
        if (r0 == 12 && blk[1] == 16) *e = (uint32_t)blk[2] << 16 | (uint32_t)blk[3] << 8 | blk[4];
        else if (r0 == 12 && blk[1] < 16) *e = rgb12(std16[blk[1]]);
        else if (r0 == 11) { blk[1] = 16; blk[2] = (uint8_t)(*e >> 16); blk[3] = (uint8_t)(*e >> 8); blk[4] = (uint8_t)*e; }
        return 0;
    }
    if (r0 == 11) {                                     /* read palette: colour -> mode 16, R, G, B */
        uint32_t v = vidc_palette[blk[0] & 0xF];
        blk[1] = 16;
        blk[2] = (uint8_t)((v & 0xF) * 17); blk[3] = (uint8_t)((v >> 4 & 0xF) * 17); blk[4] = (uint8_t)((v >> 8 & 0xF) * 17);
    }
    if (r0 == 12) {                                     /* write palette: colour, mode, R, G, B */
        unsigned colour = blk[0] & 0xF;
        vidc_palette[colour] = (blk[2] >> 4) | (uint32_t)(blk[3] >> 4) << 4 | (uint32_t)(blk[4] >> 4) << 8;
    }
    if (r0 == 22 && (blk[0] & 2))                       /* set display start */
        { display_off = (blk[1] | blk[2] << 8 | blk[3] << 16 | (uint32_t)blk[4] << 24) & 0xFFFFF; display_bank = 0; }
    return 0;
}
