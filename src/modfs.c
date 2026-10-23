/* Filing systems that modules provide: a module registers one with
 * FileSwitch (OS_FSControl 12: R1 its base, R2 the offset of its filing
 * system information block, R3 the R12 its entries get; 16 removes it, by
 * name), and names "Name:..." or "Name::disc.path" are then its files
 * (DFI's K2 holds its music in itself, in ShonkFS, a module that serves
 * named blocks of memory as files, for its tracker player to load).
 *
 * The information block (PRM 2): +0 the name, +4 the start-up text, +8
 * FSEntry_Open, +12 GetBytes, +16 PutBytes, +20 Args, +24 Close, +28 File,
 * +32 the information word, +36 Func - offsets from the module's base.
 * Files are read whole, for reading only: FSEntry_Open (R0 0, R1 the name
 * after the filing system's prefix, R3 FileSwitch's handle, R6 no special
 * field -> R1 the FS's handle, 0 if not found; R2 its buffer size, 0 for
 * an unbuffered FS; R3 the extent), GetBytes into an RMA block (R1 handle,
 * R2 address, R3 count, R4 offset: the whole file unbuffered, buffer by
 * buffer otherwise), Close; FSEntry_File 5 for the catalogue information.
 * files.c then serves the bytes as it does a host file's. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define MAXFS 8
static struct { char name[32]; uint32_t base, info, r12; } fs[MAXFS];
static int nfs;

int modfs_fscontrol(void)
{
    if (R[0] == 12) {                            /* add a filing system */
        uint32_t info = R[1] + R[2];
        if (nfs == MAXFS) { os_error(0xC5, "Too many filing systems"); return 1; }
        cstr(R[1] + RD32(info), fs[nfs].name, 1);
        fs[nfs].base = R[1]; fs[nfs].info = info; fs[nfs].r12 = R[3];
        ev("FS_added", "s", fs[nfs].name, (int)strlen(fs[nfs].name));
        for (int k = 0; k < nfs; k++)            /* (declared again: replaces the old) */
            if (!strcasecmp(fs[k].name, fs[nfs].name)) { fs[k] = fs[nfs]; return 1; }
        nfs++;
        return 1;
    }
    if (R[0] == 16) {                            /* remove one: R1 its name */
        char name[64];
        cstr(R[1], name, 1);
        for (int k = 0; k < nfs; k++)
            if (!strcasecmp(fs[k].name, name)) { memmove(fs + k, fs + k + 1, (--nfs - k) * sizeof *fs); break; }
        return 1;
    }
    return 0;
}

/* name's module filing system (-1: none) and the rest of the name (a
 * "::disc." keeps its disc: "A.Music") */
static int which(const char *name, const char **rest)
{
    while (*name == ' ') name++;                 /* (FileSwitch skips leading spaces) */
    const char *c = strchr(name, ':');
    if (!c) return -1;
    for (int k = 0; k < nfs; k++)
        if ((size_t)(c - name) == strlen(fs[k].name) && !strncasecmp(name, fs[k].name, c - name)) {
            *rest = c + 1 + (c[1] == ':');
            return k;
        }
    return -1;
}

static uint32_t entry(int k, int off) { uint32_t o = RD32(fs[k].info + off); return o ? fs[k].base + o : 0; }

int modfs_info(const char *name, uint32_t *type, uint32_t *load, uint32_t *exec, uint32_t *len)
{
    const char *rest;
    int k = which(name, &rest);
    if (k < 0) return 0;
    uint32_t r[8] = {5, roger_scratch(rest), 0, 0, 0, 0, 0, 0};
    if (!entry(k, 28) || module_call(entry(k, 28), fs[k].r12, r)) r[0] = 0;
    *type = r[0]; *load = r[2]; *exec = r[3]; *len = r[4];
    return 1;
}

uint8_t *modfs_read(const char *name, uint32_t *len, int *ours)
{
    const char *rest;
    int k = which(name, &rest);
    *ours = k >= 0;
    if (k < 0 || !entry(k, 8) || !entry(k, 12)) return NULL;
    uint32_t r[8] = {0, roger_scratch(rest), 0, 1, 0, 0, 0, 0};
    if (module_call(entry(k, 8), fs[k].r12, r) || !r[1]) return NULL;   /* FSEntry_Open */
    uint32_t h = r[1], buf = r[2], ext = r[3];
    uint32_t blk = rma_claim(ext + buf + 4);
    uint8_t *d = NULL;
    if (blk) {
        int bad = 0;
        for (uint32_t off = 0; off < ext && !bad; off += buf ? buf : ext) {   /* FSEntry_GetBytes */
            uint32_t g[8] = {0, h, blk + off, buf ? buf : ext, off, 0, 0, 0};
            bad = module_call(entry(k, 12), fs[k].r12, g) != 0;
        }
        if (!bad && (d = malloc(ext ? ext : 1))) { memcpy(d, MEM + blk, ext); *len = ext; }
        rma_free(blk);
    }
    if (entry(k, 24)) { uint32_t c[8] = {0, h, 0, 0, 0, 0, 0, 0}; module_call(entry(k, 24), fs[k].r12, c); }   /* Close */
    return d;
}
