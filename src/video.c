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
 * vertically; the output window is always the standard 256 lines. */
#include <string.h>
#include "internal.h"

/* ---------------------------------------------------------------- modes
 * The standard graphics modes of the Archimedes' 50Hz monitors. */
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
};
int screen_mode = 12;
uint32_t bank, display_bank;              /* OS_Byte 112 (VDU drivers) / 113 (display), 0-based */
static uint32_t display_off;              /* OS_Word 22: display start offset */

const struct rg_mode *mode_info(int m)
{
    m &= 0x7F;
    if (m < 0 || m >= (int)(sizeof modes / sizeof modes[0]) || !modes[m].width) return NULL;
    return &modes[m];
}
const struct rg_mode *cur_mode(void)
{
    const struct rg_mode *m = mode_info(screen_mode);
    return m ? m : &modes[12];
}
uint32_t mode_bytes(const struct rg_mode *m) { return (uint32_t)m->width * m->height * m->bpp / 8; }
uint32_t screen_size(void) { return rg->screen_size ? rg->screen_size : 0x14000; }
uint32_t screen_base(void) { return rg->screen_base ? rg->screen_base : 0x2000000u - screen_size(); }
uint32_t write_bank_addr(void) { return screen_base() + bank * mode_bytes(cur_mode()); }
void video_set_bank(int which, uint32_t b) { if (which) display_bank = b; else bank = b; }

/* VDU variables (OS_ReadVduVariables) and mode variables (OS_ReadModeVariable) */
uint32_t modevar(int mode, uint32_t v)
{
    const struct rg_mode *m = mode < 0 ? cur_mode() : mode_info(mode);
    if (!m) return 0;
    switch (v) {
    case 0: return 0;                          /* ModeFlags */
    case 1: return m->text_cols - 1;           /* ScrRCol */
    case 2: return m->text_rows - 1;           /* ScrBRow */
    case 3: return m->bpp == 8 ? 63 : (1u << m->bpp) - 1;   /* NColour (RISC OS 2: 63 in 256-colour modes) */
    case 4: return m->xeig;
    case 5: return m->yeig;
    case 6: return (uint32_t)m->width * m->bpp / 8;          /* LineLength */
    case 7: return mode_bytes(m);              /* ScreenSize */
    case 8: return 0;                          /* YShftFactor */
    case 9: case 10: return m->bpp == 8 ? 3 : m->bpp == 4 ? 2 : m->bpp == 2 ? 1 : 0;   /* Log2BPP, Log2BPC */
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
    static uint8_t q[10]; static int qn, qwant;
    if (nvdu < sizeof vdu_log) vdu_log[nvdu++] = c;
    if (rg_full_os) vdu_write(c);
    if (qwant) {
        q[qn++] = c;
        if (qn == qwant) {
            if (q[0] == 22 && mode_info(q[1])) { screen_mode = q[1] & 0x7F; video_default_palette(); }
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
 * timing, in raster lines from the start of vertical sync (bits 14-23).
 * Until the program writes them, the standard 50Hz modes' values apply. */
uint32_t vidc_palette[16];               /* 0x0BGR per entry */
uint32_t vidc_palette_top[16];           /* lines above palette_split */
int palette_split;
#define STD_VDSR 38
static int vidc_vcr = 312, vidc_vdsr = STD_VDSR, vidc_vder = STD_VDSR + 256;
static int vidc_seen[64];

void vidc_write(uint32_t v)
{
    unsigned reg = v >> 26;
    uint32_t val = v & 0x3FFFFFF;
    if (reg < 16) vidc_palette[reg] = val;
    if (reg == 40) vidc_vcr = val >> 14;
    if (reg == 43) vidc_vdsr = val >> 14;
    if (reg == 44) vidc_vder = val >> 14;
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
    const uint32_t *pal = line < palette_split ? vidc_palette_top : vidc_palette;
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
    static const uint32_t std16[16] = {
        0x000, 0x00F, 0x0F0, 0x0FF, 0xF00, 0xF0F, 0xFF0, 0xFFF,
        0x555, 0x009, 0x090, 0x099, 0x900, 0x909, 0x990, 0xAAA};
    for (int c = 0; c < 16; c++) {
        if (cur_mode()->bpp == 8) {
            int t = c & 3;
            vidc_palette[c] = (uint32_t)((c >> 2 & 1) << 2 | t) | (uint32_t)t << 4 | (uint32_t)((c >> 3 & 1) << 2 | t) << 8;
        } else vidc_palette[c] = std16[c];
    }
}

/* ---------------------------------------------------------------- MEMC
 * Writes to &3600000-&37FFFFF: the register is address bits 17-19, the
 * value address bits 2-16, a physical address in 16-byte units (the data
 * is ignored). DMA starts each frame at Vinit and, after the 16 bytes at
 * Vend, continues at Vstart. */
static uint32_t memc_vinit, memc_vstart = 0, memc_vend = ~0u;
static int memc_direct;
void memc_write(uint32_t a)
{
    uint32_t v = ((a >> 2) & 0x7FFF) << 4;
    switch ((a >> 17) & 7) {
    case 0: memc_vinit = v; memc_direct = 1; break;
    case 1: memc_vstart = v; break;
    case 2: memc_vend = v; break;
    default: break;                     /* Cinit, sound DMA, control register */
    }
}

/* The frame as the display hardware shows it: 256 lines of the current
 * mode's pixels (output lines outside VDSR..VDER are border, shown as
 * colour 0). */
const uint8_t *displayed(void)
{
    static uint8_t frame[640 * 256];
    const struct rg_mode *m = cur_mode();
    uint32_t sz = mode_bytes(m), row = sz / 256, total = screen_size();
    if (!memc_direct && vidc_vdsr == STD_VDSR && vidc_vder == STD_VDSR + 256)
        return MEM + screen_base() + display_off + display_bank * sz;
    uint32_t p = memc_direct ? memc_vinit : display_off + display_bank * sz;
    uint32_t end = memc_direct && memc_vend != ~0u ? memc_vend + 16 : total;
    int first = vidc_vdsr - STD_VDSR, last = vidc_vder - STD_VDSR;   /* output lines shown */
    for (int y = 0; y < 256; y++) {
        uint8_t *d = frame + y * row;
        if (y < first || y >= last) { memset(d, 0, row); continue; }
        for (uint32_t n = 0; n < row; n += 16) {
            memcpy(d + n, MEM + screen_base() + (p % total), 16);
            p += 16;
            if (p == end) p = memc_direct ? memc_vstart : 0;
        }
    }
    return frame;
}

/* ---------------------------------------------------------------- OS_Word */
int mbox[4] = {0, 0, 1279, 1023};               /* OS_Word 21,1: left, bottom, right, top */
static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int video_os_word(uint32_t r0, uint8_t *blk)
{
    if (r0 == 21 && blk[0] == 3 && rg_interactive)      /* set pointer position */
        display_set_mouse(clamp_i((int16_t)(blk[1] | blk[2] << 8), mbox[0], mbox[2]),
                          clamp_i((int16_t)(blk[3] | blk[4] << 8), mbox[1], mbox[3]));
    if (r0 == 21 && blk[0] == 1)                        /* define the pointer's bounding box */
        for (int k = 0; k < 4; k++) mbox[k] = (int16_t)(blk[1 + 2 * k] | blk[2 + 2 * k] << 8);
    if (r0 == 12) {                                     /* write palette: colour, mode, R, G, B */
        unsigned colour = blk[0] & 0xF;
        vidc_palette[colour] = (blk[2] >> 4) | (uint32_t)(blk[3] >> 4) << 4 | (uint32_t)(blk[4] >> 4) << 8;
    }
    if (r0 == 22 && (blk[0] & 2))                       /* set display start */
        display_off = (blk[1] | blk[2] << 8 | blk[3] << 16 | (uint32_t)blk[4] << 24) & 0xFFFFF;
    return 0;
}
