/* Relocatable modules and the RMA.
 *
 * roger_config.modules are "RMLoaded" before the program starts, as !Run
 * would: each file is loaded at its image's base in the RMA (a module is
 * position-independent, so it is translated for the address it is loaded
 * to), its initialisation entry is run, and SWIs in its chunk go to its SWI
 * handler. on_demand modules wait until the program loads them (*RMLoad,
 * *RMRun, OS_Module 0/1, matched by file), and can be killed (*RMKill,
 * OS_Module 4: finalisation entry run, code no longer dispatched); loading
 * a module whose title is already loaded replaces the old one, as RISC OS
 * does. Module code runs in the processor mode the OS would use - SVC for
 * initialisation and SWIs, IRQ for interrupt code such as a sound Level 1
 * handler - with banked R13/R14, and returns to the caller's mode through
 * R14's PSR bits. OS_Module 6 claims come from the RMA above every
 * configured module. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "internal.h"

#define MAXMOD 32
static struct {
    const struct roger_module *m;
    uint32_t base, size, chunk, private_word;
} mods[MAXMOD];                               /* loaded modules; m NULL = free slot */
static uint32_t rma_next = RMA_HEAP;

static const char *title_of(uint32_t base) { return (const char *)MEM + base + RD32(base + 0x10); }

/* run a module entry (init, final) in SVC mode with R10-R12 as given */
static int module_entry(uint32_t a, uint32_t r10, uint32_t pw)
{
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    R[10] = r10; R[11] = 0; R[12] = pw;
    VF = 0;
    call_code(a, MODE_SVC, HANDLER_RET | MODEBITS);
    int err = VF;
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    return err;
}

static void module_kill(int k)
{
    uint32_t base = mods[k].base, final = RD32(base + 8);
    ev("RMKill", "s", title_of(base), (int)strlen(title_of(base)));
    if (final && module_entry(base + final, 0, mods[k].private_word))
        fprintf(stderr, "module %s: finalisation returned an error\n", mods[k].m->path);
    cpu_remove_image(mods[k].m->image);
    mods[k].m = NULL;
}

static int module_load(const struct roger_module *m)
{
    uint32_t base = m->image->base;
    FILE *f = fopen(m->path, "rb");
    if (!f) { fprintf(stderr, "cannot load module %s\n", m->path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(n > 0x20 ? n : 0x20);
    n = (long)fread(buf, 1, n, f);
    fclose(f);
    uint32_t toff = buf[16] | buf[17] << 8 | buf[18] << 16 | (uint32_t)buf[19] << 24;
    const char *title = toff < (uint32_t)n ? (const char *)buf + toff : "";
    /* the same title already loaded: RISC OS replaces it (before the new
     * file goes in, as the two may share the address) */
    for (int j = 0; j < MAXMOD; j++)
        if (mods[j].m && !strcasecmp(title_of(mods[j].base), title)) module_kill(j);
    int k;
    for (k = 0; k < MAXMOD && mods[k].m; k++) ;
    if (k == MAXMOD) { fprintf(stderr, "too many modules\n"); free(buf); return 0; }
    memcpy(MEM + base, buf, n);
    free(buf);
    if (!m->on_demand && base + (uint32_t)n != m->image->end)
        fprintf(stderr, "warning: module %s differs from its compiled image\n", m->path);
    if (m->on_demand) ev("RMLoad", "s", title_of(base), (int)strlen(title_of(base)));
    cpu_add_image(m->image);
    uint32_t pw = MOD_PRIVATE - 4 * k;
    mods[k].m = m; mods[k].base = base; mods[k].size = (uint32_t)n; mods[k].private_word = pw;
    mods[k].chunk = RD32(base + 0x1C) & ~0x3Fu;     /* header: SWI chunk base number */
    WR32(pw, 0);
    uint32_t init = RD32(base + 4);
    if (init && module_entry(base + init, 0, pw))
        fprintf(stderr, "module %s: initialisation returned an error\n", m->path);
    return 1;
}

void modules_load(void)
{
    if (!rg->modules) return;
    /* the RMA heap starts above every module that may be loaded */
    for (const struct roger_module *m = rg->modules; m->path; m++) {
        struct stat st;
        uint32_t len = stat(m->path, &st) == 0 ? (uint32_t)st.st_size : m->image->end - m->image->base;
        uint32_t end = (m->image->base + len + 0xFFFF) & ~0xFFFFu;
        if (end > rma_next) rma_next = end;
    }
    for (const struct roger_module *m = rg->modules; m->path; m++)
        if (!m->on_demand) module_load(m);
}

/* *RMLoad / *RMRun / OS_Module 0, 1: a RISC OS filename; 0 if no
 * configured module is that file */
int modules_rmload(const char *name)
{
    char path[2048];
    struct stat a, b;
    if (!rg->modules || !hostpath(name, path) || stat(path, &b) != 0) return 0;
    for (const struct roger_module *m = rg->modules; m->path; m++)
        if (stat(m->path, &a) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            return module_load(m);
    return 0;
}

/* *RMKill / OS_Module 4: by title; 0 if not loaded */
int modules_rmkill(const char *title)
{
    for (int k = 0; k < MAXMOD; k++)
        if (mods[k].m && !strcasecmp(title_of(mods[k].base), title)) { module_kill(k); return 1; }
    return 0;
}

/* A SWI in a module's chunk: entered in SVC mode with R11 = offset in the
 * chunk, R12 = private word, R14_svc = return address + the caller's PSR,
 * which the handler's movs/orrs pc,lr restores (with V set on error).
 * R0-R9 are in/out; the kernel preserves R10-R12 for the caller. */
static void module_swi(int k, uint32_t b, int x)
{
    uint32_t handler = RD32(mods[k].base + 0x20);
    if (!handler) return;
    uint32_t r10 = R[10], r11 = R[11], r12 = R[12];
    R[11] = b & 0x3F; R[12] = mods[k].private_word;
    if ((MODEBITS & 3) == 0) bank_r13[3] = SVC_STACK;   /* a SWI from USR mode starts on an empty SVC stack */
    call_code(mods[k].base + handler, MODE_SVC, HANDLER_RET | (PSR() & ~0x10000000u));
    R[10] = r10; R[11] = r11; R[12] = r12;
    if (VF && !x) fprintf(stderr, "module SWI &%x returned an error: %s\n", b, (char *)MEM + ((R[0] + 4) & 0x3FFFFFF));
}

int modules_swi(uint32_t b, int x)
{
    for (int k = 0; k < MAXMOD; k++)
        if (mods[k].m && mods[k].chunk && (b & ~0x3Fu) == mods[k].chunk) {
            char name[24];
            sprintf(name, "Module_%x", b);
            ev(name, "iiii", R[0], R[1], R[2], R[3]);
            module_swi(k, b, x);
            return 1;
        }
    if (b == 0x1E) {                                     /* OS_Module */
        char s[1100];
        if (R[0] == 6) {                                 /* claim RMA block */
            R[2] = rma_next + 4;
            WR32(rma_next, R[3] + 4);
            rma_next = (rma_next + R[3] + 4 + 15) & ~15u;
            return 1;
        }
        ev("OS_Module", "ii", R[0], R[1]);
        if (R[0] <= 1) {                                 /* run / load: R1 filename */
            cstr(R[1], s, 1);
            if (!modules_rmload(s)) { if (x) VF = 1; else finish("OS_Module: module file not configured"); }
        } else if (R[0] == 4) {                          /* delete: R1 title */
            cstr(R[1], s, 1);
            if (!modules_rmkill(s) && x) VF = 1;
        }
        return 1;
    }
    return 0;
}
