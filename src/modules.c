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
    call_swi_code(mods[k].base + handler, MODE_SVC, HANDLER_RET | (PSR() & ~0x10000000u));
    R[10] = r10; R[11] = r11; R[12] = r12;
    if (VF && !x) fprintf(stderr, "module SWI &%x returned an error: %s\n", b, (char *)MEM + ((R[0] + 4) & 0x3FFFFFF));
}

/* *commands in loaded modules' command tables (header +&18): entries of
 * a name (zero-terminated, padded to a word), then the code offset, an
 * information word, the syntax and help offsets; a table ends with a zero
 * byte. The code runs in SVC mode with R0 the command tail and R1 the
 * number of parameters; V set on return is an error. 1 if a module has
 * the command (with code). */
int modules_command(const char *cmd, const char *tail, int x)
{
    for (int k = 0; k < MAXMOD; k++) {
        if (!mods[k].m) continue;
        uint32_t base = mods[k].base, t = RD32(base + 0x18);
        if (!t || t >= mods[k].size) continue;
        for (uint32_t e = base + t; MEM[e]; ) {
            const char *name = (const char *)MEM + e;
            uint32_t code = RD32((e + strlen(name) + 4) & ~3u);
            uint32_t next = ((e + strlen(name) + 4) & ~3u) + 16;
            if (!strcasecmp(name, cmd) && code) {
                uint32_t a = roger_scratch(tail);
                int n = 0, in = 0;
                for (const char *c = tail; *c; c++) { if (*c != ' ' && !in) n++; in = *c != ' '; }
                ev("module_command", "s", name, (int)strlen(name));
                uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
                memcpy(saved, R, sizeof R);
                R[0] = a; R[1] = n; R[12] = mods[k].private_word;
                VF = 0;
                call_swi_code(base + code, MODE_SVC, HANDLER_RET | MODEBITS);
                int err = VF;
                uint32_t r0 = R[0];
                memcpy(R, saved, sizeof R);
                NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
                if (err) {
                    fprintf(stderr, "*%s: %s\n", name, (char *)MEM + ((r0 + 4) & 0x3FFFFFF));
                    if (x) { VF = 1; R[0] = r0; }
                }
                return 1;
            }
            e = next;
        }
    }
    return 0;
}

/* A service call to every module with a service entry (header +&0C), in
 * SVC mode with R12 its private word and R0-R9 as given, until one claims
 * it (R1 = 0). The registers are left as the modules returned them; 1 if
 * claimed. */
int modules_service(void)
{
    for (int k = 0; k < MAXMOD && R[1]; k++) {
        if (!mods[k].m) continue;
        uint32_t entry = RD32(mods[k].base + 0x0C);
        if (!entry) continue;
        uint32_t r10 = R[10], r11 = R[11], r12 = R[12];
        R[12] = mods[k].private_word;
        call_code(mods[k].base + entry, MODE_SVC, HANDLER_RET | MODEBITS);
        R[10] = r10; R[11] = r11; R[12] = r12;
    }
    return R[1] == 0;
}

int modules_swi(uint32_t b, int x)
{
    if (b == 0x30) { modules_service(); return 1; }          /* OS_ServiceCall */
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
        } else if (R[0] == 18) {                         /* look up by name: R1 the name (to a space */
            cstr(R[1], s, 1);                            /* or control character; "%instance" ignored) */
            s[strcspn(s, " %")] = 0;
            uint32_t base, word;
            for (int k = 0; k < MAXMOD; k++)
                if (mods[k].m && !strcasecmp(title_of(mods[k].base), s)) {
                    R[1] = k; R[2] = 0; R[3] = mods[k].base; R[4] = RD32(mods[k].private_word); R[5] = 0;
                    return 1;
                }
            if (disc_rom_module(s, &base, &word)) {      /* RISC OS's own (stand-ins: disc.c) */
                R[1] = MAXMOD + (base >> 10 & 15); R[2] = 0; R[3] = base; R[4] = RD32(word); R[5] = 0;
                return 1;
            }
            os_error(0x102, "Module not found");
        }
        return 1;
    }
    return 0;
}

/* OS_SWINumberFromString for module SWIs: each loaded module's SWI decoding
 * table (header +&24: the prefix, then one name per SWI from its chunk,
 * ending with an empty name). "Prefix_Name" -> chunk + index. */
int modules_swi_number(const char *name, uint32_t *n)
{
    for (int k = 0; k < MAXMOD; k++) {
        uint32_t off = mods[k].m ? RD32(mods[k].base + 0x24) : 0;
        if (!off || off >= mods[k].size || !RD32(mods[k].base + 0x1C)) continue;   /* (none, or not a table) */
        const char *t = (const char *)MEM + mods[k].base + off;
        size_t pl = strnlen(t, mods[k].size - off);
        if (strncmp(name, t, pl) || name[pl] != '_') continue;
        const char *p = t + pl + 1;
        const char *end = (const char *)MEM + mods[k].base + mods[k].size;
        for (uint32_t i = 0; p < end && *p && i < 64; i++, p += strnlen(p, end - p) + 1)
            if (!strcmp(name + pl + 1, p)) { *n = RD32(mods[k].base + 0x1C) + i; return 1; }
    }
    return 0;
}
