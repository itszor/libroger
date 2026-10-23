/* SpriteExtend's JPEG SWIs (RISC OS 3.6 on), as the PRMs give them:
 *   &49980 JPEG_Info (R0 flags: bit 0 dimensions only; R1 the image, R2
 *     its length) -> R0 bit 0 greyscale, bit 2 its pixel density is only a
 *     ratio; R2, R3 width and height; R4, R5 pixel density; R6 the
 *     workspace a plot needs (unless R0 bit 0 was set)
 *   &49981 JPEG_FileInfo: the same, R1 a filename
 *   &49982 JPEG_PlotScaled (R0 the image, R1, R2 its bottom left in OS
 *     units, R3 scale factors as OS_SpriteOp 52's or 0, R4 its length, R5
 *     flags: dithering)
 *   &49983 JPEG_PlotFileScaled (R0 a filename, R1-R3 as above, R4 flags)
 * Decoded with stb_image (Debian's libstb-dev: public domain, header
 * only), plotted by the VDU drivers (vdu_plot_rgb) in any depth, without
 * dithering; drawn only when the VDU drivers draw (interactive runs,
 * --full-os). The transformed plots (&49984, &49985) aren't done. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb/stb_image.h>
#pragma GCC diagnostic pop

/* (not SpriteExtend's own error number, which isn't known here) */
static void bad_jpeg(void) { os_error(0x1E6, "Bad JPEG file"); }

/* a file's contents (malloc'd), or NULL with an error given */
static uint8_t *read_named(uint32_t name, uint32_t *len)
{
    char rname[256], path[2048];
    cstr(name, rname, 1);
    FILE *f = hostpath_read(rname, path) ? fopen(path, "rb") : NULL;
    if (!f) { os_error(0xD6, "File not found"); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? n : 1);
    *len = (uint32_t)fread(d, 1, n > 0 ? n : 0, f);
    fclose(f);
    return d;
}

/* the JFIF header's density (APP0: units 0 a ratio, 1 dpi, 2 dots per
 * cm), 90 dpi without one */
static void density(const uint8_t *p, uint32_t n, int *ratio, int *xd, int *yd)
{
    *ratio = 0, *xd = *yd = 90;
    if (n >= 18 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF && p[3] == 0xE0 && !memcmp(p + 6, "JFIF", 5)) {
        *ratio = p[13] == 0;
        *xd = p[14] << 8 | p[15];
        *yd = p[16] << 8 | p[17];
    }
}

static void info(const uint8_t *p, uint32_t n, uint32_t flags)
{
    int w, h, comp, ratio, xd, yd;
    if (!stbi_info_from_memory(p, (int)n, &w, &h, &comp)) { bad_jpeg(); return; }
    density(p, n, &ratio, &xd, &yd);
    R[0] = (comp == 1) | ratio << 2;
    R[2] = (uint32_t)w, R[3] = (uint32_t)h, R[4] = (uint32_t)xd, R[5] = (uint32_t)yd;
    if (!(flags & 1)) R[6] = (uint32_t)w * 16 * 3 + 20000;   /* a strip of MCUs and tables */
}

static void plot(const uint8_t *p, uint32_t n, int x, int y, uint32_t fac)
{
    int w, h, comp;
    if (!stbi_info_from_memory(p, (int)n, &w, &h, &comp)) { bad_jpeg(); return; }
    ev("JPEG_Plot", "iiii", (uint32_t)w, (uint32_t)h, (uint32_t)x, (uint32_t)y);
    if (!rg_full_os) return;
    uint8_t *rgb = stbi_load_from_memory(p, (int)n, &w, &h, &comp, 3);
    if (!rgb) { bad_jpeg(); return; }
    vdu_plot_rgb(x, y, w, h, rgb, fac);
    stbi_image_free(rgb);
}

int jpeg_swi(uint32_t b)
{
    uint8_t *d;
    uint32_t n;
    switch (b) {
    case 0x49980:
        if (R[1] + (uint64_t)R[2] > MEMSIZE) { bad_jpeg(); return 1; }
        info(MEM + R[1], R[2], R[0]);
        return 1;
    case 0x49981:
        if ((d = read_named(R[1], &n))) { info(d, n, R[0]); free(d); }
        return 1;
    case 0x49982:
        if (R[0] + (uint64_t)R[4] > MEMSIZE) { bad_jpeg(); return 1; }
        plot(MEM + R[0], R[4], (int32_t)R[1], (int32_t)R[2], R[3]);
        return 1;
    case 0x49983:
        if ((d = read_named(R[0], &n))) { plot(d, n, (int32_t)R[1], (int32_t)R[2], R[3]); free(d); }
        return 1;
    }
    return 0;
}
