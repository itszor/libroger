/* Relocatable modules and the RMA.
 *
 * roger_config.modules are "RMLoaded" before the program starts, as !Run
 * would: each file is loaded at its image's base in the RMA (a module is
 * position-independent, so it is translated for the address it is loaded
 * to), its initialisation entry is run, and SWIs in its chunk go to its SWI
 * handler. Module code runs in the processor mode the OS would use - SVC
 * for initialisation and SWIs, IRQ for interrupt code such as a sound
 * Level 1 handler - with banked R13/R14, and returns to the caller's mode
 * through R14's PSR bits. OS_Module 6 claims come from the RMA after them. */
#include <string.h>
#include "internal.h"

#define MAXMOD 8
static struct { uint32_t base, size, chunk, private_word; } mods[MAXMOD];
static int nmods;
static uint32_t rma_next = RMA_HEAP;

static void module_load(const struct roger_module *m, int k)
{
    uint32_t base = m->image->base;
    long n = load_file(m->path, base);
    if (n <= 0) { fprintf(stderr, "cannot load module %s\n", m->path); return; }
    if (base + (uint32_t)n != m->image->end)
        fprintf(stderr, "warning: module %s differs from its compiled image\n", m->path);
    cpu_add_image(m->image);
    uint32_t pw = MOD_PRIVATE - 4 * k;
    mods[nmods].base = base; mods[nmods].size = (uint32_t)n; mods[nmods].private_word = pw;
    mods[nmods].chunk = RD32(base + 0x1C) & ~0x3Fu;     /* header: SWI chunk base number */
    nmods++;
    uint32_t end = (base + (uint32_t)n + 0xFFFF) & ~0xFFFFu;
    if (end > rma_next) rma_next = end;
    WR32(pw, 0);
    uint32_t init = RD32(base + 4);
    if (!init) return;
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    R[10] = 0; R[11] = 0; R[12] = pw;              /* init: environment string, instantiation, private word */
    VF = 0;
    call_code(base + init, MODE_SVC, HANDLER_RET | MODEBITS);
    if (VF) fprintf(stderr, "module %s: initialisation returned an error\n", m->path);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
}

void modules_load(void)
{
    if (!rg->modules) return;
    for (int k = 0; rg->modules[k].path && nmods < MAXMOD; k++) module_load(&rg->modules[k], k);
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
    for (int k = 0; k < nmods; k++)
        if (mods[k].chunk && (b & ~0x3Fu) == mods[k].chunk) {
            char name[24];
            sprintf(name, "Module_%x", b);
            ev(name, "iiii", R[0], R[1], R[2], R[3]);
            module_swi(k, b, x);
            return 1;
        }
    if (b == 0x1E) {                                     /* OS_Module */
        if (R[0] == 6) {                                 /* claim RMA block */
            R[2] = rma_next + 4;
            WR32(rma_next, R[3] + 4);
            rma_next = (rma_next + R[3] + 4 + 15) & ~15u;
        } else ev("OS_Module", "ii", R[0], R[1]);
        return 1;
    }
    return 0;
}
