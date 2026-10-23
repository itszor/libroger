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
 * does. in_memory modules are inserted where the program put them
 * (OS_Module 10), with no file, or copied from there into the RMA, to
 * their image's base (OS_Module 11), known by their title. Module code runs in the processor mode the OS would use - SVC for
 * initialisation and SWIs, IRQ for interrupt code such as a sound Level 1
 * handler - with banked R13/R14, and returns to the caller's mode through
 * R14's PSR bits. OS_Module 6 claims come from the RMA above every
 * configured module: a block's size (including that word) in the word
 * before it, as RISC OS's heap keeps it, and each block on a 16-byte
 * boundary; OS_Module 7 frees one (a later claim may reuse it) and 13
 * extends or shrinks one (moving it if need be). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "internal.h"

#define MAXMOD 32
#define MAXINC 8
static struct {
    const struct roger_module *m;
    uint32_t base, size, chunk, private_word;   /* private_word: the preferred incarnation's */
    struct { char postfix[32]; uint32_t pw; } inc[MAXINC];   /* its incarnations ("Base" first) */
    int ninc;
} mods[MAXMOD];                               /* loaded modules; m NULL = free slot */

/* Incarnations (OS_Module 14-16, "Title%Postfix"): the first, "Base", is
 * the one loading makes, with its private word at MOD_PRIVATE - 4k; others
 * have theirs at INC_WORDS. SWIs and *commands go to the preferred one,
 * service calls to each. */
#define INC_WORDS 0x017FF000u

/* "Title%Postfix" -> the module and incarnation (-1: no postfix); 0 if no
 * such module (or, with a postfix, incarnation) */
static int find_inc(const char *spec, int *km, int *ki)
{
    char t[64];
    size_t n = strcspn(spec, "% ");
    snprintf(t, sizeof t, "%.*s", (int)(n < 63 ? n : 63), spec);
    for (int k = 0; k < MAXMOD; k++)
        if (mods[k].m && !strcasecmp((const char *)MEM + mods[k].base + RD32(mods[k].base + 0x10), t)) {
            *km = k; *ki = -1;
            if (spec[n] != '%') return 1;
            char pf[32];
            size_t l = strcspn(spec + n + 1, " ");
            snprintf(pf, sizeof pf, "%.*s", (int)(l < 31 ? l : 31), spec + n + 1);
            for (int i = 0; i < mods[k].ninc; i++) if (!strcasecmp(mods[k].inc[i].postfix, pf)) { *ki = i; return 1; }
            return 0;
        }
    return 0;
}
static uint32_t rma_next = RMA_HEAP;

/* the RMA's freed blocks: [a, a + n) from a block's size word, sorted */
#define MAXFREE 1024
static struct { uint32_t a, n; } rfree[MAXFREE];
static int nrfree;
static uint32_t rma_len(uint32_t size) { return (size + 4 + 15) & ~15u; }   /* a claim's extent */
uint32_t rma_size(void) { return rma_next - RMA_BASE; }

uint32_t rma_claim(uint32_t size)
{
    uint32_t len = rma_len(size);
    for (int k = 0; k < nrfree; k++)
        if (rfree[k].n >= len) {
            uint32_t a = rfree[k].a;
            rfree[k].a += len; rfree[k].n -= len;
            if (!rfree[k].n) { memmove(rfree + k, rfree + k + 1, (nrfree - k - 1) * sizeof *rfree); nrfree--; }
            WR32(a, size + 4);
            return a + 4;
        }
    uint32_t a = rma_next;
    WR32(a, size + 4);
    rma_next += len;
    return a + 4;
}

void rma_free(uint32_t blk)
{
    uint32_t a = blk - 4, n = rma_len(RD32(a) - 4);
    if (a < RMA_HEAP || a + n > rma_next) return;
    int k = 0;
    while (k < nrfree && rfree[k].a < a) k++;
    if (k > 0 && rfree[k - 1].a + rfree[k - 1].n > a) return;             /* (already free) */
    if (k > 0 && rfree[k - 1].a + rfree[k - 1].n == a) { a = rfree[k - 1].a; n += rfree[k - 1].n; k--; nrfree--;
        memmove(rfree + k, rfree + k + 1, (nrfree - k) * sizeof *rfree); }
    if (k < nrfree && a + n == rfree[k].a) { n += rfree[k].n; nrfree--;
        memmove(rfree + k, rfree + k + 1, (nrfree - k) * sizeof *rfree); }
    if (a + n == rma_next) { rma_next = a; return; }                    /* the top: given back */
    if (nrfree == MAXFREE) return;                                      /* (lost) */
    memmove(rfree + k + 1, rfree + k, (nrfree - k) * sizeof *rfree);
    rfree[k].a = a; rfree[k].n = n;
    nrfree++;
}

static uint32_t rma_extend(uint32_t blk, int32_t by)
{
    uint32_t a = blk - 4, old = RD32(a) - 4, size = (int32_t)(old + by) > 0 ? old + by : 0;
    if (rma_len(size) <= rma_len(old)) { WR32(a, size + 4); return blk; }   /* fits where it is */
    if (a + rma_len(old) == rma_next) { WR32(a, size + 4); rma_next = a + rma_len(size); return blk; }
    uint32_t nb = rma_claim(size);
    memmove(MEM + nb, MEM + blk, old);
    rma_free(blk);
    return nb;
}

static const char *title_of(uint32_t base) { return (const char *)MEM + base + RD32(base + 0x10); }

/* run a module entry (init, final) in SVC mode with R10-R12 as given */
/* a module's initialisation or finalisation entry: its error block, or 0.
 * R11: the instantiation number (0 the first, Base) */
static uint32_t module_entry_n(uint32_t a, uint32_t r10, uint32_t pw, uint32_t r11)
{
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    R[10] = r10; R[11] = r11; R[12] = pw;
    VF = 0;
    call_code(a, MODE_SVC, HANDLER_RET | MODEBITS);
    uint32_t err = VF ? (R[0] ? R[0] : ERROR_BUF) : 0;
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    return err;
}
static uint32_t module_entry(uint32_t a, uint32_t r10, uint32_t pw) { return module_entry_n(a, r10, pw, 0); }

/* Module code at a called in SVC mode with R0-R7 from r (and set from
 * them on return) and R12 r12, as FileSwitch calls a filing system's
 * entries: the error block, or 0 */
uint32_t module_call(uint32_t a, uint32_t r12, uint32_t r[8])
{
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    memcpy(R, r, 8 * sizeof *r);
    R[12] = r12;
    VF = 0;
    call_code(a, MODE_SVC, HANDLER_RET | MODEBITS);
    uint32_t err = VF ? (R[0] ? R[0] : ERROR_BUF) : 0;
    memcpy(r, R, 8 * sizeof *r);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    return err;
}
static const char *error_text(uint32_t e) { return (const char *)MEM + ((e + 4) & 0x3FFFFFF); }

/* one incarnation finalised and gone (the last: the module with it) */
static void inc_kill(int k, int i)
{
    uint32_t base = mods[k].base, final = RD32(base + 8), pw = mods[k].inc[i].pw, e;
    if (final && (e = module_entry(base + final, 0, pw)))
        fprintf(stderr, "module %s: finalisation returned an error: %s\n", mods[k].m->path, error_text(e));
    int preferred = mods[k].private_word == pw;
    memmove(mods[k].inc + i, mods[k].inc + i + 1, (--mods[k].ninc - i) * sizeof mods[k].inc[0]);
    if (preferred && mods[k].ninc) mods[k].private_word = mods[k].inc[0].pw;
    if (!mods[k].ninc) { cpu_remove_image(mods[k].m->image); mods[k].m = NULL; }
}

static void module_kill(int k)
{
    uint32_t base = mods[k].base;
    ev("RMKill", "s", title_of(base), (int)strlen(title_of(base)));
    while (mods[k].m) inc_kill(k, mods[k].ninc - 1);
}

/* m's n bytes at buf (NULL: already in place, in_memory) put in at its
 * image's base and initialised */
static int module_insert(const struct roger_module *m, const uint8_t *buf, uint32_t n, const char *args)
{
    uint32_t base = m->image->base;
    const uint8_t *hdr = buf ? buf : MEM + base;
    uint32_t toff = hdr[16] | hdr[17] << 8 | hdr[18] << 16 | (uint32_t)hdr[19] << 24;
    char title[64] = "";
    if (toff < n) snprintf(title, sizeof title, "%s", (const char *)hdr + toff);
    /* the same title already loaded: RISC OS replaces it (before the new
     * file goes in, as the two may share the address) */
    for (int j = 0; j < MAXMOD; j++)
        if (mods[j].m && !strcasecmp(title_of(mods[j].base), title)) module_kill(j);
    int k;
    for (k = 0; k < MAXMOD && mods[k].m; k++) ;
    if (k == MAXMOD) { fprintf(stderr, "too many modules\n"); return 0; }
    if (buf) memcpy(MEM + base, buf, n);
    if (!m->on_demand && !m->in_memory && base + n != m->image->end)
        fprintf(stderr, "warning: module %s differs from its compiled image\n", m->path);
    if (m->on_demand || m->in_memory) ev("RMLoad", "s", title_of(base), (int)strlen(title_of(base)));
    cpu_add_image(m->image);
    uint32_t pw = MOD_PRIVATE - 4 * k;
    mods[k].m = m; mods[k].base = base; mods[k].size = n; mods[k].private_word = pw;
    mods[k].chunk = RD32(base + 0x1C) & ~0x3Fu;     /* header: SWI chunk base number */
    strcpy(mods[k].inc[0].postfix, "Base");
    mods[k].inc[0].pw = pw;
    mods[k].ninc = 1;
    WR32(pw, 0);
    uint32_t init = RD32(base + 4);
    uint32_t e;
    /* R10: the parameters (the environment string), as RISC OS gives them */
    if (!args) args = m->args ? m->args : "";
    if (init && (e = module_entry(base + init, roger_scratch(args), pw)))
        fprintf(stderr, "module %s: initialisation returned an error: %s\n", m->path, error_text(e));
    return 1;
}

static int module_load(const struct roger_module *m, const char *args)
{
    FILE *f = fopen(m->path, "rb");
    if (!f) { fprintf(stderr, "cannot load module %s\n", m->path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = calloc(1, n > 0x20 ? n : 0x20);
    n = (long)fread(buf, 1, n, f);
    fclose(f);
    int ok = module_insert(m, buf, (uint32_t)n, args);
    free(buf);
    return ok;
}

void modules_load(void)
{
    if (!rg->modules) return;
    /* the RMA heap starts above every module that may be loaded */
    for (const struct roger_module *m = rg->modules; m->path; m++) {
        struct stat st;
        if (m->in_memory == 1) continue;                 /* (wherever the program puts it) */
        if (m->in_memory == 2) {                         /* (copied to its image's base) */
            uint32_t end = (m->image->end + 0xFFFF) & ~0xFFFFu;
            if (end > rma_next) rma_next = end;
            continue;
        }
        uint32_t len = stat(m->path, &st) == 0 ? (uint32_t)st.st_size : m->image->end - m->image->base;
        uint32_t end = (m->image->base + len + 0xFFFF) & ~0xFFFFu;
        if (end > rma_next) rma_next = end;
    }
    for (const struct roger_module *m = rg->modules; m->path; m++)
        if (!m->on_demand && !m->in_memory) module_load(m, NULL);
}

/* *RMLoad / *RMRun / OS_Module 0, 1: a RISC OS filename and the
 * parameters after it (NULL: the configuration's); 0 if no configured
 * module is that file */
int modules_rmload(const char *name, const char *args)
{
    char path[2048];
    struct stat a, b;
    if (!rg->modules || !hostpath(name, path) || stat(path, &b) != 0) return 0;
    for (const struct roger_module *m = rg->modules; m->path; m++)
        if (!m->in_memory && stat(m->path, &a) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            return module_load(m, args);
    return 0;
}

/* A module of RISC OS's own that libroger implements, as a header in "ROM"
 * (ROM_MODULES on) for programs that look it up (OS_Module 18, *RMEnsure):
 * its title, help string (with the version), and a SWI handler that makes
 * the SWI again (the runtime handles it), with the decoding table */
void rom_module(uint32_t base, const char *title, const char *help, uint32_t chunk, const char *const *swis)
{
    memset(MEM + base, 0, 0x400);
    WR32(base + 0x10, 0x30);                         /* title */
    strcpy((char *)MEM + base + 0x30, title);
    WR32(base + 0x1C, chunk);
    WR32(base + 0x20, 0x40);                         /* SWI handler */
    WR32(base + 0x40, 0xE92D4000);                   /* stmfd sp!, {lr} */
    WR32(base + 0x44, 0xE08FF18B);                   /* add pc, pc, r11, lsl #3 */
    WR32(base + 0x48, 0xE1A00000);                   /* nop */
    uint32_t exit = base + 0x4C + 8 * 64;
    for (uint32_t n = 0; n < 64; n++) {
        uint32_t b = base + 0x50 + 8 * n;
        WR32(b - 4, 0xEF000000 | (chunk + n) | 0x20000);
        WR32(b, 0xEA000000 | (((exit - (b + 8)) >> 2) & 0xFFFFFF));   /* b exit */
    }
    WR32(exit, 0xE8BD4000);                          /* ldmfd sp!, {lr} */
    WR32(exit + 4, 0x639EF201);                      /* orrvss pc, lr, #&10000000 */
    WR32(exit + 8, 0xE3DEF201);                      /* bics pc, lr, #&10000000 */
    uint32_t p = exit + 12;                          /* the SWI decoding table */
    WR32(base + 0x24, swis ? p - base : 0);
    for (const char *const *s = swis; s; s++) {
        size_t l = *s ? strlen(*s) : 0;
        memcpy(MEM + p, *s ? *s : "", l + 1);
        p += l + 1;
        if (!*s) break;
    }
    if (help) { WR32(base + 0x14, p - base); strcpy((char *)MEM + p, help); }
}

/* the OS's own modules (rom_module), after disc.c's FileCore and ADFS */
#define OS_MODULES (ROM_MODULES + 0x1000)
static const char *const os_titles[] = {"WindowManager", "MessageTrans", "ResourceFS", "TerritoryManager",
                                        "SharedCLibrary", "UtilityModule", "ColourTrans", NULL};
void modules_init(void)
{
    char wimp[64];
    snprintf(wimp, sizeof wimp, "Window Manager\t%d.%02d (18 Feb 1998)", wimp_version() / 100, wimp_version() % 100);
    rom_module(OS_MODULES, "WindowManager", wimp, 0x400C0, NULL);
    rom_module(OS_MODULES + 0x400, "MessageTrans", "MessageTrans\t0.23 (24 Jan 1992)", 0x41500,
               (const char *const[]){"MessageTrans", "FileInfo", "OpenFile", "Lookup", "MakeMenus", "CloseFile",
                                     "EnumerateTokens", "ErrorLookup", "GSLookup", "CopyError", NULL});
    rom_module(OS_MODULES + 0x800, "ResourceFS", "ResourceFS\t0.15 (09 Nov 1992)", 0x41B40,
               (const char *const[]){"ResourceFS", "RegisterFiles", "DeregisterFiles", NULL});
    rom_module(OS_MODULES + 0xC00, "TerritoryManager", "Territory Manager\t0.23 (19 Oct 1992)", 0x43040, NULL);
    rom_module(OS_MODULES + 0x1000, "SharedCLibrary", "SharedCLibrary\t5.17 (12 Mar 1998)", 0x80680,
               (const char *const[]){"SharedCLibrary", "LibInitAPCS_A", "LibInitAPCS_R", "LibInitModule", NULL});
    rom_module(OS_MODULES + 0x1400, "UtilityModule", "RISC OS\t3.10 (30 Apr 1992)", 0, NULL);
    rom_module(OS_MODULES + 0x1800, "ColourTrans", "Colour Selector\t1.16 (20 Aug 1992)", 0x40740, NULL);
}

/* OS_Module 18 for the OS's own modules (and disc.c's): 1 if name is one */
static int rom_module_find(const char *name, uint32_t *base, uint32_t *word)
{
    if (disc_rom_module(name, base, word)) return 1;
    for (int k = 0; os_titles[k]; k++)
        if (!strcasecmp(os_titles[k], name)) { *base = OS_MODULES + 0x400 * k; *word = 0; return 1; }
    return 0;
}

/* A version number as *RMEnsure and help strings give it ("1.36"): in
 * hundredths */
int version_hundredths(const char *v)
{
    int n = 0, f = 0, d = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + *v++ - '0';
    if (*v == '.') for (v++; *v >= '0' && *v <= '9'; v++) if (d < 2) f = f * 10 + *v - '0', d++;
    if (d == 1) f *= 10;
    return n * 100 + f;
}

/* a loaded module's version (its help string's, after the title), in
 * hundredths; -1 if it isn't loaded */
int modules_version(const char *title)
{
    uint32_t base = 0, word;
    for (int k = 0; k < MAXMOD && !base; k++)
        if (mods[k].m && !strcasecmp(title_of(mods[k].base), title)) base = mods[k].base;
    if (!base && !rom_module_find(title, &base, &word)) return -1;
    if (!RD32(base + 0x14)) return 0;
    /* "Title<tabs or spaces>1.36 (date)": the first number after white
     * space (a title may have spaces, or start with a digit) */
    const char *h = (const char *)MEM + base + RD32(base + 0x14);
    for (const char *p = h + 1; *p; p++)
        if ((p[-1] == ' ' || p[-1] == '\t') && *p >= '0' && *p <= '9') return version_hundredths(p);
    return 0;
}

/* *RMKill / OS_Module 4: by title; 0 if not loaded */
int modules_rmkill(const char *title)
{
    for (int k = 0; k < MAXMOD; k++)
        if (mods[k].m && !strcasecmp(title_of(mods[k].base), title)) { module_kill(k); return 1; }
    return 0;
}

/* *RMClear: every module killed, the last loaded first */
void modules_rmclear(void)
{
    for (int k = MAXMOD - 1; k >= 0; k--)
        if (mods[k].m) module_kill(k);
}

/* A SWI in a module's chunk: entered in SVC mode with R11 = offset in the
 * chunk, R12 = private word, R14_svc = the kernel's return address with
 * the caller's flags and interrupt state but SVC mode, as RISC OS calls
 * it: a handler that enables interrupts with `bic rX, lr, #I : teqp rX, #0`
 * stays in SVC mode (LZWD, Saloon Cars'). It returns with movs/orrs pc,lr
 * (V set on error); the caller's mode is restored afterwards. R0-R9 are
 * in/out; the kernel preserves R10-R12 for the caller. */
static void module_swi(int k, uint32_t b, int x)
{
    uint32_t handler = RD32(mods[k].base + 0x20);
    if (!handler) return;
    uint32_t r10 = R[10], r11 = R[11], r12 = R[12];
    R[11] = b & 0x3F; R[12] = mods[k].private_word;
    if ((MODEBITS & 3) == 0) bank_r13[3] = SVC_STACK;   /* a SWI from USR mode starts on an empty SVC stack */
    call_swi_code(mods[k].base + handler, MODE_SVC, HANDLER_RET | (PSR() & ~0x10000003u) | 3);
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
/* a service call from the OS itself (R1 the reason, R2 its argument), the
 * caller's registers and flags kept */
void modules_service_call(uint32_t reason, uint32_t r2)
{
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, snpc = npc;
    memcpy(saved, R, sizeof R);
    R[1] = reason; R[2] = r2;
    ev("service", "ii", reason, r2);
    modules_service();
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    npc = snpc;
}

int modules_service(void)
{
    for (int k = 0; k < MAXMOD && R[1]; k++) {
        if (!mods[k].m) continue;
        uint32_t entry = RD32(mods[k].base + 0x0C);
        if (!entry) continue;
        for (int i = 0; i < mods[k].ninc && R[1] && mods[k].m; i++) {   /* each incarnation */
            uint32_t r10 = R[10], r11 = R[11], r12 = R[12];
            R[12] = mods[k].inc[i].pw;
            call_code(mods[k].base + entry, MODE_SVC, HANDLER_RET | MODEBITS);
            R[10] = r10; R[11] = r11; R[12] = r12;
        }
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
        if (R[0] == 6) { R[2] = rma_claim(R[3]); return 1; }               /* claim: R3 size -> R2 */
        ev("OS_Module", "ii", R[0], R[1]);
        if (R[0] == 7) { rma_free(R[2]); return 1; }                      /* free: R2 */
        if (R[0] == 13) { R[2] = rma_extend(R[2], (int32_t)R[3]); return 1; }   /* extend: R2 by R3 */
        if (R[0] <= 1) {                                 /* run / load: R1 filename */
            cstr(R[1], s, 1);
            char *params = s + strcspn(s, " ");          /* (the filename, then its parameters) */
            if (*params) *params++ = 0;
            while (*params == ' ') params++;
            if (!modules_rmload(s, params)) { if (x) VF = 1; else finish("OS_Module: module file not configured"); }
        } else if (R[0] == 10) {                         /* insert from memory: R1 the module, in */
            const struct roger_module *m = NULL;         /* place, its length (at most) the word before */
            for (const struct roger_module *c = rg->modules; c && c->path && !m; c++)
                if (c->in_memory == 1 && c->image->base == R[1]) m = c;
            if (!m) {
                if (x) os_error(0x103, "Module not configured");
                else finish("OS_Module 10: no in_memory module configured at that address");
            } else if (!module_insert(m, NULL, RD32(R[1] - 4), NULL) && x) os_error(0x101, "No room in module area");
        } else if (R[0] == 11) {                         /* insert from memory, copying it into the */
            const struct roger_module *m = NULL;         /* RMA: R1 the module, R2 its length */
            uint32_t toff = RD32(R[1] + 16);
            cstr(R[1] + toff, s, 1);
            for (const struct roger_module *c = rg->modules; c && c->path && !m; c++)
                if (c->in_memory == 2 && !strcasecmp(c->path, s)) m = c;
            if (!m || toff >= R[2]) {
                if (x) os_error(0x103, "Module not configured");
                else finish("OS_Module 11: no in_memory 2 module configured with that title");
            } else if (R[2] > m->image->end - m->image->base + 0x10000) {
                if (x) os_error(0x101, "No room in module area"); else finish("OS_Module 11: module too long");
            } else if (!module_insert(m, MEM + R[1], R[2], NULL) && x) os_error(0x101, "No room in module area");
        } else if (R[0] == 4) {                          /* delete: R1 title (%postfix: the incarnation) */
            int k, i;
            cstr(R[1], s, 1);
            if (strchr(s, '%')) {
                if (find_inc(s, &k, &i) && i >= 0) inc_kill(k, i);
                else if (x) os_error(0x10B, "Incarnation not found");
            } else if (!modules_rmkill(s) && x) os_error(0x102, "Module not found");
        } else if (R[0] == 14) {                         /* new incarnation: R1 "Title%Postfix [args]" */
            int k = -1, i = -1;
            cstr(R[1], s, 1);
            if (!strchr(s, '%')) { os_error(0x10D, "Incarnation name required"); return 1; }
            int there = find_inc(s, &k, &i);              /* (k set if the module is loaded) */
            if (k < 0) { os_error(0x102, "Module not found"); return 1; }
            if (there) { os_error(0x10C, "Incarnation already exists"); return 1; }
            if (mods[k].ninc == MAXINC) { os_error(0x101, "No room for another incarnation"); return 1; }
            int n = mods[k].ninc;
            const char *pf = strchr(s, '%') + 1, *args = pf + strcspn(pf, " ");
            snprintf(mods[k].inc[n].postfix, sizeof mods[k].inc[n].postfix, "%.*s", (int)(args - pf), pf);
            uint32_t pw = INC_WORDS + 4 * (k * MAXINC + n), init = RD32(mods[k].base + 4), e;
            for (int j = 0; j < n; j++) if (mods[k].inc[j].pw == pw) pw += 4 * MAXINC * MAXMOD;   /* (reused) */
            mods[k].inc[n].pw = pw;
            mods[k].ninc++;
            WR32(pw, 0);
            while (*args == ' ') args++;
            if (init && (e = module_entry_n(mods[k].base + init, roger_scratch(args), pw, n))) {
                mods[k].ninc--;
                R[0] = e; VF = 1;
            }
        } else if (R[0] == 15 || R[0] == 16) {           /* rename (R2 the new postfix) / make preferred */
            int k, i;
            cstr(R[1], s, 1);
            if (!find_inc(s, &k, &i)) { os_error(0x10B, "Incarnation not found"); return 1; }
            if (i < 0) i = 0;
            if (R[0] == 16) mods[k].private_word = mods[k].inc[i].pw;
            else { char n[32]; cstr(R[2], n, 1); snprintf(mods[k].inc[i].postfix, 32, "%s", n); }
        } else if (R[0] == 12) {                         /* enumerate with private data: R1 module, R2 */
            int k = (int)R[1];                           /* incarnation -> R1, R2 the next, R3 base, R4 */
            while (k < MAXMOD && !mods[k].m) k++;        /* private word's value, R5 postfix */
            if (k >= MAXMOD || (int)R[2] >= mods[k].ninc) { os_error(0x107, "No more modules"); return 1; }
            int i = (int)R[2];
            strcpy((char *)MEM + MOD_POSTFIX, mods[k].inc[i].postfix);
            R[3] = mods[k].base; R[4] = RD32(mods[k].inc[i].pw); R[5] = MOD_POSTFIX;
            if (i + 1 < mods[k].ninc) R[2] = i + 1; else { R[1] = k + 1; R[2] = 0; }
        } else if (R[0] == 18) {                         /* look up by name: R1 the name (to a space */
            cstr(R[1], s, 1);                            /* or control character; "%instance" ignored) */
            s[strcspn(s, " %")] = 0;
            uint32_t base, word;
            for (int k = 0; k < MAXMOD; k++)
                if (mods[k].m && !strcasecmp(title_of(mods[k].base), s)) {
                    R[1] = k; R[2] = 0; R[3] = mods[k].base; R[4] = RD32(mods[k].private_word); R[5] = 0;
                    return 1;
                }
            if (rom_module_find(s, &base, &word)) {      /* RISC OS's own (stand-ins) */
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
