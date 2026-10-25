/* The emulated CPU around the translated code: state, processor modes,
 * dispatch, the fallback ARM2 interpreter and diagnostics.
 *
 * Translated code runs as C functions F_xxxx(entry); any PC write that
 * isn't a branch within the function stores the target in npc and returns,
 * and run_until() dispatches npc through call_addr(). An address with no
 * translated entry is a gap: known gaps (roger_config.interpreted) are
 * interpreted, anything else stops the run - finding out about a gap when
 * it's hit is what drives what gets translated next - unless --trace-gaps
 * (log each gap once and interpret) or interpret_gaps is set. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

uint32_t R[16];
uint32_t NF, ZF, CF, VF;
uint8_t *MEM;
uint32_t npc;
uint32_t pc_bias;
uint64_t icount, irq_next;
const uint8_t *codemap;
uint32_t img_base = LOAD, img_end;

int in_handler, interp_only;
uint64_t interp_count, smc_count;

/* ---------------------------------------------------------------- modes
 * 26-bit ARM: R13/R14 are banked per mode, R8-R12 too in FIQ. Programs run
 * in USR mode; module code runs in SVC mode (SWIs, init) and interrupt code
 * in IRQ mode, and switch between them themselves with TEQP, relying on
 * the banking (e.g. a module's IRQ code switching to SVC around a SWI,
 * which corrupts R14_svc but not R14_irq). */
uint32_t MODEBITS;
uint32_t bank_r13[4], bank_r14[4];
static uint32_t bank_fiq[5], bank_r8[5];
void set_psr_mode(uint32_t m)
{
    uint32_t from = MODEBITS & 3, to = m & 3;
    if (from != to) {
        bank_r13[from] = R[13]; bank_r14[from] = R[14];
        if (from == 1) { memcpy(bank_fiq, R + 8, sizeof bank_fiq); memcpy(R + 8, bank_r8, sizeof bank_r8); }
        if (to == 1) { memcpy(bank_r8, R + 8, sizeof bank_r8); memcpy(R + 8, bank_fiq, sizeof bank_fiq); }
        R[13] = bank_r13[to]; R[14] = bank_r14[to];
    }
    MODEBITS = m & 0x0C000003;
}

/* The USR mode registers, from a privileged mode (LDM/STM with the S bit
 * and no PC: "^" transfers the user bank): R13-R14 banked in every other
 * mode, R8-R12 in FIQ mode too */
uint32_t user_reg(int k)
{
    uint32_t m = MODEBITS & 3;
    if (!m || k < 8 || k == 15 || (k < 13 && m != 1)) return R[k];
    return k == 13 ? bank_r13[0] : k == 14 ? bank_r14[0] : bank_r8[k - 8];
}

void set_user_reg(int k, uint32_t v)
{
    uint32_t m = MODEBITS & 3;
    if (!m || k < 8 || k == 15 || (k < 13 && m != 1)) R[k] = v;
    else if (k == 13) bank_r13[0] = v;
    else if (k == 14) bank_r14[0] = v;
    else bank_r8[k - 8] = v;
}

/* ---------------------------------------------------------------- images
 * The program image (where &8000-based code is dispatched; replaced when a
 * chain-loaded program takes over) plus any others, e.g. modules in the
 * RMA, dispatched wherever their address range is. */
struct loaded { const struct roger_image *img; fn_t *tab; int overlay; const uint8_t *orig; };
#define MAXIMG 64
static struct loaded prog, extra[MAXIMG];
static int nextra;

static fn_t *build_tab(const struct roger_image *img)
{
    fn_t *tab = calloc((img->end - img->base) / 4 + 1, sizeof(fn_t));
    for (unsigned k = 0; k < img->nentries; k++) tab[(img->entries[k].a - img->base) >> 2] = img->entries[k].f;
    return tab;
}

static fn_t *tab_for(const struct roger_image *img)
{
    static struct loaded cache[MAXIMG];
    for (int k = 0; k < MAXIMG; k++) {
        if (cache[k].img == img) return cache[k].tab;
        if (!cache[k].img) { cache[k].img = img; return cache[k].tab = build_tab(img); }
    }
    return build_tab(img);
}

void roger_set_image(const struct roger_image *img)
{
    prog.img = img;
    prog.tab = tab_for(img);
    prog.orig = NULL;
    img_base = img->base; img_end = img->end; codemap = img->codemap;
    (void)COV_TAG(img->cov_tag ? img->cov_tag : 'P');
}
const struct roger_image *roger_current_image(void) { return prog.img; }

/* The image an address is dispatched through: of the images loaded beside
 * the program (modules, overlays), the one loaded last that covers it - an
 * overlay loaded over another, or over part of the program, replaces the
 * code there - and otherwise the program. NULL if none covers it. */
static struct loaded *owner(uint32_t a)
{
    for (int k = nextra - 1; k >= 0; k--)
        if (a >= extra[k].img->base && a < extra[k].img->end) return &extra[k];
    if (a >= img_base && a < img_end) return &prog;
    return NULL;
}

/* Would dispatching a reach translated code? (relocations applied) */
int cpu_translated(uint32_t a)
{
    if (rg->relocations)
        for (const struct roger_relocation *r = rg->relocations; r->image; r++)
            if (prog.img == r->image && a >= r->to && a < r->to + r->len) a -= r->to - r->from;
    struct loaded *l = owner(a);
    return l && l->tab[(a - l->img->base) >> 2] != 0;
}

/* Stores into the images beside the program are checked for self-modified
 * code as the program's are (CODECHK): a module may patch itself (Desktop
 * Tracker sets its mixers' output stride for the tune's channels) */
uint32_t extra_lo, extra_hi;
static void extra_span(void)
{
    extra_lo = extra_hi = 0;
    for (int k = 0; k < nextra; k++) {
        if (!k || extra[k].img->base < extra_lo) extra_lo = extra[k].img->base;
        if (extra[k].img->end > extra_hi) extra_hi = extra[k].img->end;
    }
}

void smc_write_extra(uint32_t a)
{
    a &= 0x3FFFFFC;
    for (int k = nextra - 1; k >= 0; k--) {
        const struct roger_image *img = extra[k].img;
        if (a >= img->base && a < img->end) {
            if (img->codemap[(a - img->base) >> 5] >> (((a - img->base) >> 2) & 7) & 1) smc_write(a);
            return;
        }
    }
}

void cpu_add_image(const struct roger_image *img)
{
    if (nextra < MAXIMG) {
        extra[nextra].img = img; extra[nextra].tab = tab_for(img); extra[nextra].overlay = 0; extra[nextra].orig = NULL;
        nextra++;
    }
    extra_span();
}

void cpu_remove_image(const struct roger_image *img)          /* (keeping the others' order) */
{
    for (int k = 0; k < nextra; k++)
        if (extra[k].img == img) { memmove(&extra[k], &extra[k + 1], (--nextra - k) * sizeof *extra); break; }
    extra_span();
}

/* Programs that may be loaded at their image's base (or, packed, at their
 * `at`): OS_File loaded one; it takes over when control next arrives at
 * its base. A program's leaf names the file; with dots in it, the last
 * components of its name ("Data.CODE": two files of the same leaf). */
static const struct roger_image *pending;
static uint32_t pending_entry;            /* where it starts */
static const uint8_t *pending_orig;

/* A program unpacked in place (roger_program.unpacked): the image's words
 * as translated, read once from the file; and, as its packed file is
 * loaded, every compiled word marked dirty that doesn't hold its unpacked
 * value now, so that code dispatched there before the unpacking has put
 * it back is interpreted as it is */
static const uint8_t *unpacked_words(const struct roger_program *p)
{
    static struct { const struct roger_program *p; uint8_t *w; } cache[MAXIMG];
    int k;
    for (k = 0; k < MAXIMG && cache[k].p; k++)
        if (cache[k].p == p) return cache[k].w;
    uint32_t n = p->image->end - p->image->base;
    uint8_t *w = calloc(n, 1);
    FILE *f = fopen(p->unpacked, "rb");
    if (!f || fread(w, 1, n, f) == 0) { fprintf(stderr, "%s: can't read it\n", p->unpacked); finish("no unpacked image"); }
    fclose(f);
    if (k < MAXIMG) cache[k].p = p, cache[k].w = w;
    return w;
}
static void mark_unpacked(const struct roger_image *img, const uint8_t *orig)
{
    for (uint32_t a = img->base; a < img->end; a += 4)
        if (img->codemap[(a - img->base) >> 5] >> (((a - img->base) >> 2) & 7) & 1) {
            uint32_t w;
            memcpy(&w, orig + (a - img->base), 4);
            if (RD32(a) != w) smc_dirty[a >> 5] |= 1 << ((a >> 2) & 7);
            else smc_dirty[a >> 5] &= ~(1 << ((a >> 2) & 7));
        }
}
static int names_program(const char *name, const char *leaf)
{
    char x[1100];
    gstrans(name, x, sizeof x);
    size_t ln = strlen(x), ll = strlen(leaf);
    if (!strchr(leaf, '.')) {
        const char *l = strrchr(x, '.');
        return !strcasecmp(l ? l + 1 : x, leaf);
    }
    return ln >= ll && !strcasecmp(x + ln - ll, leaf) && (ln == ll || strchr(".:", x[ln - ll - 1]));
}
void cpu_programs_loaded(const char *name, uint32_t addr)
{
    if (!rg->programs) return;
    for (const struct roger_program *p = rg->programs; p->leaf; p++)
        if (names_program(name, p->leaf) && (p->at ? p->at : p->image->base) == addr) {
            const uint8_t *orig = p->unpacked ? unpacked_words(p) : NULL;
            if (!p->overlay) {
                pending = p->image; pending_entry = p->entry ? p->entry : p->image->base;
                pending_orig = orig;
            } else {
                cpu_remove_image(p->image);
                cpu_add_image(p->image);
                if (nextra && extra[nextra - 1].img == p->image) {
                    extra[nextra - 1].overlay = 1;
                    extra[nextra - 1].orig = orig;
                    if (orig) mark_unpacked(p->image, orig);
                }
            }
        }
}
void roger_program_loaded(const char *name, uint32_t addr) { cpu_programs_loaded(name, addr); }

/* ---------------------------------------------------------------- dispatch */
static int smc_stale(uint32_t a);
static int seen(uint32_t *tab, int *n, int max, uint32_t a)
{
    for (int k = 0; k < *n; k++) if (tab[k] == a) return 1;
    if (*n < max) tab[(*n)++] = a;
    return 0;
}
int cpu_seen_gap(uint32_t a) { static uint32_t s[4096]; static int n; return seen(s, &n, 4096, a); }

/* untranslated.log: one line per gap address, the first time it's reached,
 * with the image it's in ("img=base-end", or "img=none"), which tells apart
 * images translated for the same address. "from pc" is the instruction
 * executed before it: a gap word reached from the gap word before it is the
 * interpreter stepping on, anything else is an entry into untranslated code
 * (SuperSpider's tools/gap_roots.py makes those roots for the next analysis). */
void cpu_log_gap(uint32_t a)
{
    FILE *f = fopen("untranslated.log", "a");
    if (f) {
        uint32_t b = 0, e = 0;
        struct loaded *l = owner(a);
        if (l) b = l->img->base, e = l->img->end;
        fprintf(f, "%llu: branch to untranslated address %08x (LR=%08x, from pc=%08x)",
                (unsigned long long)icount, a, R[14], g_cur_pc);
        if (e) fprintf(f, " img=%08x-%08x\n", b, e); else fprintf(f, " img=none\n");
        fclose(f);
    }
    fprintf(stderr, "branch to untranslated address %08x (icount %llu, LR=%08x, from pc=%08x) - see untranslated.log\n",
            a, (unsigned long long)icount, R[14], g_cur_pc);
}

/* Execution gates: code addresses whose first execution the runtime acts
 * on (--crop's, host.c). An armed gate's word is marked dirty, so that
 * translated code leaves through the dispatcher there, as at self-modified
 * code; the gate then fires once and is gone. At a word that isn't an entry
 * of the translation, the code from there is interpreted on until it
 * reaches one. A store to the word (self-modification) disarms its gate. */
#define MAXGATES 64
static struct { uint32_t a; void (*fn)(uint32_t); } gates[MAXGATES];
static int ngates;

int cpu_gate(uint32_t a, void (*fn)(uint32_t))
{
    a &= 0x3FFFFFC;
    if (ngates == MAXGATES || SMC_DIRTY(a)) return 0;
    gates[ngates].a = a; gates[ngates++].fn = fn;
    smc_dirty[a >> 5] |= 1 << ((a >> 2) & 7);
    return 1;
}

/* the gate at a (disarmed), or NULL */
static void (*gate_take(uint32_t a))(uint32_t)
{
    for (int k = 0; k < ngates; k++)
        if (gates[k].a == a) {
            void (*fn)(uint32_t) = gates[k].fn;
            gates[k] = gates[--ngates];
            smc_dirty[a >> 5] &= ~(1 << ((a >> 2) & 7));
            return fn;
        }
    return NULL;
}

void call_addr(uint32_t a)
{
    if (a == RETURN_TO_OS) finish("returned to OS");
    if (a == OS_SWI_ENTRY) { os_swi_entry(); return; }
    if (rg->call_addr && rg->call_addr(a)) return;
    if (clib_dispatch(a)) return;             /* the Shared C Library (clib.c) */
    if (rg->relocations)
        for (const struct roger_relocation *r = rg->relocations; r->image; r++)
            if (prog.img == r->image && a >= r->to && a < r->to + r->len) {
                pc_bias = r->to - r->from;
                a -= pc_bias;
                npc = a;
            }
    if (pending && a == pending_entry) {
        fprintf(stderr, "chain-load: program at %08x replaced (icount %llu)\n", a, (unsigned long long)icount);
        roger_set_image(pending);
        if ((prog.orig = pending_orig)) mark_unpacked(pending, pending_orig);
        pending = NULL;
        pc_bias = 0;
    }
    void (*gate)(uint32_t);
    if (ngates && SMC_DIRTY(a) && (gate = gate_take(a))) {
        ev("gate", "i", a);
        gate(a);
        if (!interp_only && !cpu_translated(a)) {      /* inside a translated function: */
            int n = 0;                                 /* on to where it carries on */
            do interp_step();
            while (++n < 1000 && npc != HANDLER_RET && npc != RETURN_TO_OS && !cpu_translated(npc));
            if (n == 1000) fprintf(stderr, "gate &%X: no translated code after it\n", a);
            return;
        }
    }
    /* self-modified: as it is now; or back as it was, but not an entry of
       the translation (translated code left at it, the word dirty: a
       program unpacked in place, part way through) - as translated */
    if (SMC_DIRTY(a) && (smc_stale(a) || !cpu_translated(a))) { interp_step(); return; }
    fn_t f = 0;
    static uint32_t dbg_lo = 1, dbg_hi;           /* ROGER_INTERP=from:to (hex): interpret that range,
                                                    to tell a translation fault from the program's */
    if (dbg_lo == 1) {
        const char *e = getenv("ROGER_INTERP");
        dbg_lo = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
        dbg_hi = e && strchr(e, ':') ? (uint32_t)strtoul(strchr(e, ':') + 1, NULL, 16) : 0;
    }
    if (a >= dbg_lo && a < dbg_hi) {
        static uint32_t peek = 1;                 /* ROGER_PEEKPC=addr (hex, in that range): the
                                                     registers each time it's reached */
        if (peek == 1) { const char *e = getenv("ROGER_PEEKPC"); peek = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (a == peek) fprintf(stderr, "PEEK %llu pc=%x r0=%x r1=%x r2=%x r3=%x r4=%x r5=%x r6=%x\n", (unsigned long long)icount, a, R[0], R[1], R[2], R[3], R[4], R[5], R[6]);
        interp_step(); return;
    }
    if (!interp_only) {
        struct loaded *l = nextra || !(a >= img_base && a < img_end) ? owner(a) : &prog;
        if (l) f = l->tab[(a - l->img->base) >> 2];
    }
    if (f) { f(a); return; }
    if (a == HANDLER_RET) return;
    if (interp_only) { interp_step(); return; }   /* a loader program: all of it interpreted */
    if (a >= ROM_MODULES && a < ROM_MODULES_END) { interp_step(); return; }   /* (disc.c) */
    if (rg->interpreted)
        for (const uint32_t *g = rg->interpreted; g[1]; g += 2)
            if (a >= g[0] && a < g[1]) { interp_step(); return; }   /* a known gap */
    if (rg->interpret_gaps) {                 /* --trace-gaps still logs them */
        if (rg_trace_gaps && !cpu_seen_gap(a)) cpu_log_gap(a);
        interp_step();
        return;
    }
    struct loaded *l = owner(a);              /* the program's code: its image, or an overlay */
    int program = l && (l == &prog || l->overlay);
    if (l && !program && !rg_trace_gaps) {
        /* module code should be fully translated: report the gap and end
         * this call rather than the whole run (with --trace-gaps it is
         * interpreted, as the program's is, while its roots are found) */
        if (!cpu_seen_gap(a)) cpu_log_gap(a);
        npc = HANDLER_RET;
        return;
    }
    if (in_handler && (!rg_trace_gaps || (!program && !l))) {
        /* an interrupt handler the runtime called landed somewhere it can't
         * follow: end that call (the next interrupt tries again). With
         * --trace-gaps only untranslated code in the program is followed: a
         * handler chaining to an old vector the runtime left at 0 would
         * otherwise have the interpreter walk through zero page for good. */
        if (!cpu_seen_gap(a)) cpu_log_gap(a);
        npc = HANDLER_RET;
        return;
    }
    if (rg_trace_gaps) {
        if (!cpu_seen_gap(a)) cpu_log_gap(a);
        interp_step();
        return;
    }
    cpu_log_gap(a);
    finish("branch to untranslated code");
}

void run_until(uint32_t stop)
{
    while (npc != stop) {
        if (npc == RETURN_TO_OS) finish("returned to OS");
        if (npc == HANDLER_RET) finish("return to the runtime outside a call (a gap in module code?)");
        call_addr(npc);
    }
}
void roger_run_until(uint32_t stop) { run_until(stop); }

/* Run code at addr in processor mode `mode` with R14 = r14 until it returns
 * to HANDLER_RET (interrupt handlers, voice generators, module entries).
 * No interrupts are taken meanwhile, unless it is interruptible: a SWI
 * handler, which RISC OS runs with interrupts enabled (and which may wait
 * for an interrupt handler to change something) - not if it's called from
 * an interrupt handler itself. */
static void run_code(uint32_t addr, uint32_t mode, uint32_t r14, int interruptible)
{
    uint32_t snpc = npc, smode = MODEBITS;
    int sh = in_handler;
    if (!interruptible) in_handler = 1;
    set_psr_mode(mode);
    R[14] = r14;
    npc = addr;
    COV_ASYNC_ENTER();
    uint64_t i0 = icount;
    while (npc != HANDLER_RET) {
        if (npc == RETURN_TO_OS) finish("returned to OS");
        if (in_handler && icount - i0 > 50000000) {
            fprintf(stderr, "call_code(%08x): bailing out after 50M instructions, pc=%08x\n", addr, npc);
            break;
        }
        call_addr(npc);
    }
    COV_ASYNC_EXIT();
    set_psr_mode(smode);
    npc = snpc;
    in_handler = sh;
}
void call_code(uint32_t addr, uint32_t mode, uint32_t r14) { run_code(addr, mode, r14, 0); }
void call_swi_code(uint32_t addr, uint32_t mode, uint32_t r14) { run_code(addr, mode, r14, 1); }
void roger_call(uint32_t addr, uint32_t mode, uint32_t r14) { call_code(addr, mode, r14); }
/* Code the program runs as itself (a translated BASIC program's CALL):
 * interrupts are taken meanwhile, as for the program proper. */
void roger_call_program(uint32_t addr, uint32_t r14) { run_code(addr, 0, r14, 1); }
int roger_in_handler(void) { return in_handler; }

void bad_entry(uint32_t fn, uint32_t e)
{
    fprintf(stderr, "bad entry %08x into F_%x\n", e, fn);
    finish("bad entry");
}
/* A program's own handler in one of the ARM's hardware vectors (&4, &8,
 * ...): where the branch or ldr pc,[pc,#n] it put there leads, or 0 - the
 * OS's, which the runtime is. */
uint32_t hw_vector(uint32_t v)
{
    uint32_t w = RD32(v);
    if ((w & 0xFF000000u) == 0xEA000000u) return (v + 8 + ((int32_t)(w << 8) >> 6)) & 0x3FFFFFC;
    if ((w & 0xFFFFF000u) == 0xE59FF000u) return RD32(v + 8 + (w & 0xFFF)) & 0x3FFFFFC;
    return 0;
}

/* An undefined instruction - on an ARM2, a coprocessor's too, there being
 * none: the trap, as RISC OS passes it on to the undefined instruction
 * handler (OS_ChangeEnvironment 1) in SVC mode with IRQs off and R14_svc
 * the address after it (with the caller's PSR). Programs telling an ARM2
 * from an ARM3 (an MRC from the cache controller, p15) rely on it. The
 * floating point instructions are the FPEmulator's (fpa.c). With no
 * handler, the run stops. */
void undefined_insn(uint32_t a, uint32_t w)
{
    /* the ARM's undefined instruction vector, &4: RISC OS's leads to the
       OS_ChangeEnvironment 1 handler; a program may put its own branch
       there (Desktop Tracker does, to get from IRQ to SVC mode) */
    uint32_t to = hw_vector(4);
    if (to) {
        uint32_t ret = ((a + 4) & 0x3FFFFFC) | PSR();
        ev("undefined_instruction", "ii", a, w);
        set_psr_mode(0x08000000u | 3);
        R[14] = ret;
        npc = to;
        return;
    }
    /* RISC OS's FPEmulator, on the vector, takes the FPA's instructions
       (fpa.c) before the OS passes the rest on */
    if (fpa_insn(a, w)) return;
    if (env[1][0]) {
        uint32_t ret = ((a + 4) & 0x3FFFFFC) | PSR();
        ev("undefined_instruction", "ii", a, w);
        set_psr_mode(0x08000000u | 3);
        R[14] = ret;
        npc = env[1][0];
        return;
    }
    fprintf(stderr, "undefined/coproc instruction %08x at %08x\n", w, a);
    finish("undefined instruction");
}
/* Self-modified compiled code. A store to a compiled word (CODECHK, before
 * the store) marks it dirty and, the first time, records the word it
 * held: the one it was translated from. Translated code leaves through
 * the dispatcher at a dirty word (I()); dispatching it interprets it,
 * unless it holds its original value again. In a program unpacked in
 * place (roger_program.unpacked) the original is the unpacked image's
 * word, whatever was there before. */
uint8_t smc_dirty[MEMSIZE / 32];
#define SMC_ORIG 65536
static struct { uint32_t a, w; } smc_orig[SMC_ORIG];      /* open addressing; a = 0 free */

static uint32_t *orig_slot(uint32_t a, int add)
{
    unsigned h = (a >> 2) % SMC_ORIG;
    for (int k = 0; k < SMC_ORIG; k++, h = (h + 1) % SMC_ORIG) {
        if (smc_orig[h].a == a) return &smc_orig[h].w;
        if (!smc_orig[h].a) {
            if (!add) return NULL;
            smc_orig[h].a = a; smc_orig[h].w = RD32(a);
            return &smc_orig[h].w;
        }
    }
    return NULL;
}

void smc_write(uint32_t a)
{
    a &= 0x3FFFFFC;
    if (SMC_DIRTY(a) && !(ngates && gate_take(a))) return;
    struct loaded *l = owner(a);
    if (!(l && l->orig) && !orig_slot(a, 1)) return;     /* (table full: left as it was) */
    smc_dirty[a >> 5] |= 1 << ((a >> 2) & 7);
    static uint32_t s[64]; static int n;
    if (seen(s, &n, 64, a)) return;
    smc_count++;
    fprintf(stderr, "note: store to compiled code at %08x (icount %llu)\n", a, (unsigned long long)icount);
}

/* a dirty word dispatched: 1 if it differs from what was translated (so
 * interpret it); one back to its original value is clean again */
static int smc_stale(uint32_t a)
{
    struct loaded *l = owner(a);
    uint32_t w, *o = orig_slot(a, 0);
    if (l && l->orig) memcpy(o = &w, l->orig + (a - l->img->base), 4);
    if (o && RD32(a) == *o) { smc_dirty[a >> 5] &= ~(1 << ((a >> 2) & 7)); return 0; }
    return 1;
}

/* ---------------------------------------------------------------- diagnostics
 * PC histogram (after the first 700000 instructions), printed at exit:
 * where the program spends its time when it seems stuck. Sampled at each
 * interrupt point outside handlers (irq_point: every 20000 instructions
 * in interactive runs), not counted at every instruction, which cost
 * translated code a call each. Open-addressed with a bounded probe, so a PC that finds no
 * slot is just not counted. */
#define PCHIST 65536
#define PCHIST_PROBE 32
static struct { uint32_t a; uint64_t n; } pchist[PCHIST];
uint32_t g_cur_pc;
void pchist_sample(void)
{
    uint32_t a = g_cur_pc;
    if (icount < 700000) return;
    unsigned h = (a >> 2) % PCHIST;
    for (int k = 0; k < PCHIST_PROBE; k++, h = (h + 1) % PCHIST)
        if (!pchist[h].n || pchist[h].a == a) { pchist[h].a = a; pchist[h].n++; return; }
}
void cpu_dump_pchist(void)
{
    for (int pass = 0; pass < 15; pass++) {
        uint32_t besta = 0; uint64_t bestn = 0; int besti = -1;
        for (int k = 0; k < PCHIST; k++)
            if (pchist[k].n > bestn) { bestn = pchist[k].n; besta = pchist[k].a; besti = k; }
        if (besti < 0) break;
        fprintf(stderr, "pchist #%d: %08x x%llu\n", pass, besta, (unsigned long long)bestn);
        pchist[besti].n = 0;
    }
}
void cpu_report_state(const char *why)
{
    fprintf(stderr, "%s: pc=%08x npc=%08x icount=%llu in_handler=%d mode=%u img=%08x-%08x\n",
            why, g_cur_pc, npc, (unsigned long long)icount, in_handler, MODEBITS & 3, img_base, img_end);
}

#ifdef RECOMP_TRACE
/* RECOMP_TRACE=from:to  ->  rec.trace: "<icount> <pc> r0..r14 psr" per instruction */
static uint64_t tr_from = ~0ull, tr_to;
static FILE *trf;
void trace_insn(uint32_t a)
{
    if (!trf && tr_from == ~0ull && getenv("RECOMP_TRACE")) {
        tr_from = (uint64_t)strtod(getenv("RECOMP_TRACE"), 0);
        tr_to = (uint64_t)strtod(strchr(getenv("RECOMP_TRACE"), ':') + 1, 0);
    }
    if (icount < tr_from || icount >= tr_to) return;
    if (!trf) trf = fopen("rec.trace", "w");
    fprintf(trf, "%llu %x", (unsigned long long)icount, a);
    for (int k = 0; k < 15; k++) fprintf(trf, " %x", R[k]);
    fprintf(trf, " %x\n", PSR());
    fflush(trf);
}
#endif

/* ---------------------------------------------------------------- interpreter
 * One ARM2 instruction at npc, for gaps and for self-modified instructions
 * whose current form wasn't translated. */
static uint32_t ipc;                     /* address of the instruction being interpreted */

/* The ARM2's pipeline: while an instruction executes, the next two words
 * have already been fetched, so a store to either isn't seen when they run
 * in sequence after it - only once the pipeline is refilled (a branch, an
 * exception, a SWI's return). Copy protection relies on it (Phaethon's
 * str r1,[pc] in a loop that patches the program, which an emulator
 * without it runs differently). pf_at: the address the words in pf_w were
 * fetched for; pf_ic: the instruction count when they were, so that
 * anything run in between (translated code, an interrupt) refills it. */
static uint32_t pf_at = 1, pf_w[2];
static uint64_t pf_ic;

static int cond_ok(uint32_t c)
{
    switch (c) {
    case 0: return ZF; case 1: return !ZF; case 2: return CF; case 3: return !CF;
    case 4: return NF; case 5: return !NF; case 6: return VF; case 7: return !VF;
    case 8: return CF && !ZF; case 9: return !CF || ZF; case 10: return NF == VF;
    case 11: return NF != VF; case 12: return !ZF && NF == VF; case 13: return ZF || NF != VF;
    case 14: return 1; default: return 0;
    }
}
static uint32_t getr(int n, int pcoff, int full)
{
    if (n == 15) { uint32_t p = (ipc + pcoff) & 0x3FFFFFC; return full ? p | PSR() : p; }
    return R[n];
}
static uint32_t shift_imm(uint32_t v, int typ, uint32_t amt, uint32_t *c)
{
    switch (typ) {
    case 0: if (amt == 0) { *c = CF; return v; } *c = (v >> (32 - amt)) & 1; return v << amt;
    case 1: if (amt == 0) amt = 32;
            if (amt == 32) { *c = v >> 31; return 0; } *c = (v >> (amt - 1)) & 1; return v >> amt;
    case 2: if (amt == 0) amt = 32;
            if (amt >= 32) { *c = v >> 31; return *c ? 0xFFFFFFFF : 0; }
            *c = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt);
    default: if (amt == 0) { *c = v & 1; return (CF << 31) | (v >> 1); }
             *c = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
    }
}
static uint32_t reg_op(uint32_t i, uint32_t *c)
{
    int rm = i & 15, typ = (i >> 5) & 3;
    if (i & 0x10) {
        uint32_t amt = getr((i >> 8) & 15, 8, 1) & 0xFF;
        return shr_val(getr(rm, 12, 1), typ, amt, c);
    }
    return shift_imm(getr(rm, 8, 1), typ, (i >> 7) & 31, c);
}
static void setpc_i(uint32_t v, int s) { npc = v & 0x3FFFFFC; if (s) SETPSR(v); }

static int exec_word(uint32_t pc, uint32_t i)
{
    ipc = pc;
    uint32_t next = (pc + 4) & 0x3FFFFFC;
    npc = next;
    if (!cond_ok(i >> 28)) return 0;
    uint32_t op = (i >> 25) & 7;
    /* MUL/MLA: the ARM2 takes bits 7 and 4 set (bits 27-22 clear) as a
     * multiply, whatever bits 6-5 are - &0000FFFF is muleq r0,pc,pc.
     * Diggers calls one, with Z set, a word before a routine (an
     * assembler's slip); as andeq pc,r0,pc,ror pc it would jump away */
    if (op <= 1 && (i & 0x0FC00090) == 0x00000090) {
        int rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
        uint32_t v = R[rm] * R[rs];
        if (i & (1 << 21)) v += R[rn];
        R[rd] = v;
        if (i & (1 << 20)) SETNZ(v);
    } else if (op <= 1 && (i & 0x0F8000F0) == 0x00800090) {   /* UMULL/UMLAL/SMULL/SMLAL (ARMv3M) */
        int hi = (i >> 16) & 15, lo = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
        uint64_t v = i & (1 << 22) ? (uint64_t)((int64_t)(int32_t)R[rm] * (int32_t)R[rs])
                                   : (uint64_t)R[rm] * R[rs];
        if (i & (1 << 21)) v += (uint64_t)R[hi] << 32 | R[lo];
        R[lo] = (uint32_t)v;
        R[hi] = (uint32_t)(v >> 32);
        if (i & (1 << 20)) { NF = (uint32_t)(v >> 63); ZF = v == 0; }
    } else if (op <= 1 && (i & 0x0FB00FF0) == 0x01000090) {   /* SWP */
        int rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15;
        uint32_t a = R[rn], t;
        if (i & (1 << 22)) { t = RD8(a); WR8(a, R[rm]); } else { t = RD32(a); WR32(a, R[rm]); }
        R[rd] = t;
    } else if (op <= 1) {                                      /* data processing */
        uint32_t opc = (i >> 21) & 15, s = (i >> 20) & 1, rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        uint32_t op2, sc, c = CF, v = VF, res;
        if (i & (1 << 25)) {
            uint32_t rot = ((i >> 8) & 15) * 2;
            op2 = rot ? ((i & 0xFF) >> rot) | ((i & 0xFF) << (32 - rot)) : (i & 0xFF);
            sc = rot ? op2 >> 31 : CF;
        } else op2 = reg_op(i, &sc);
        int regshift = !(i & (1 << 25)) && (i & 0x10);
        uint32_t a = getr(rn, regshift ? 12 : 8, 0);
        int logical = 1;
        uint64_t t;
        #define ADDC(x, y, ci) (t = (uint64_t)(x) + (y) + (ci), res = (uint32_t)t, c = (uint32_t)(t >> 32), \
                                v = (((x) ^ res) & ((y) ^ res)) >> 31, logical = 0)
        switch (opc) {
        case 0: case 8: res = a & op2; break;
        case 1: case 9: res = a ^ op2; break;
        case 2: case 10: ADDC(a, ~op2, 1); break;
        case 3: ADDC(op2, ~a, 1); break;
        case 4: case 11: ADDC(a, op2, 0); break;
        case 5: ADDC(a, op2, CF); break;
        case 6: ADDC(a, ~op2, CF); break;
        case 7: ADDC(op2, ~a, CF); break;
        case 12: res = a | op2; break;
        case 13: res = op2; break;
        case 14: res = a & ~op2; break;
        default: res = ~op2;
        }
        #undef ADDC
        if (logical) c = sc;
        if ((s && rd != 15) || (s && opc >= 8 && opc <= 11)) {
            if (opc >= 8 && opc <= 11 && rd == 15) SETPSR(res);
            else { NF = res >> 31; ZF = res == 0; CF = c; VF = v; }
        }
        if (opc >= 8 && opc <= 11) return 0;
        if (rd == 15) { setpc_i(res, s); return 1; }
        R[rd] = res;
    } else if (op == 3 && (i & 0x10)) {                       /* the undefined instruction space */
        undefined_insn(pc, i);
        return 1;
    } else if (op <= 3) {                                      /* LDR/STR */
        uint32_t p = (i >> 24) & 1, u = (i >> 23) & 1, bb = (i >> 22) & 1, w = (i >> 21) & 1,
                 l = (i >> 20) & 1, rn = (i >> 16) & 15, rd = (i >> 12) & 15, off, dummy;
        if (i & (1 << 25)) off = reg_op(i, &dummy); else off = i & 0xFFF;
        uint32_t base = getr(rn, 8, 0);
        uint32_t addr = u ? base + off : base - off, ea = p ? addr : base, val = 0;
        if (l) val = bb ? RD8(ea) : RD32(ea);
        else {
            val = rd == 15 ? ((pc + 12) & 0x3FFFFFC) | PSR() : R[rd];
            if (bb) WR8(ea, val); else WR32(ea, val);
        }
        if ((!p || w) && rn != 15) R[rn] = addr;
        if (l) { if (rd == 15) { setpc_i(val, 0); return 1; } R[rd] = val; }
    } else if (op == 4) {                                      /* LDM/STM */
        uint32_t p = (i >> 24) & 1, u = (i >> 23) & 1, s = (i >> 22) & 1, w = (i >> 21) & 1,
                 l = (i >> 20) & 1, rn = (i >> 16) & 15;
        int n = __builtin_popcount(i & 0xFFFF);
        uint32_t base = rn != 15 ? R[rn] : pc + 8, start, wbv;
        if (u) { start = base + (p ? 4 : 0); wbv = base + 4 * n; }
        else { start = base - 4 * n + (p ? 0 : 4); wbv = base - 4 * n; }
        uint32_t a = start & ~3u;                              /* LDM/STM ignore address bits 0-1 */
        int jumped = 0;
        int user = s && !(l && (i & 0x8000));                  /* ^ without a PC load: the user bank */
        if (l) {
            if (w && rn != 15) R[rn] = wbv;
            for (int k = 0; k < 16; k++) if (i & (1 << k)) {
                uint32_t v2 = RD32(a);
                if (k == 15) { setpc_i(v2, s); jumped = 1; }
                else if (user) set_user_reg(k, v2); else R[k] = v2;
                a += 4;
            }
        } else {
            for (int k = 0; k < 16; k++) if (i & (1 << k)) {
                WR32(a, k == 15 ? ((pc + 12) & 0x3FFFFFC) | PSR() : user ? user_reg(k) : R[k]);
                a += 4;
            }
            if (w && rn != 15) R[rn] = wbv;
        }
        return jumped;
    } else if (op == 5) {                                      /* B/BL */
        int32_t off = i & 0xFFFFFF;
        if (off & 0x800000) off -= 0x1000000;
        if (i & (1 << 24)) R[14] = next | PSR();
        npc = (pc + 8 + off * 4) & 0x3FFFFFC;
        return 1;
    } else if (op == 7 && (i >> 24) & 1) {                     /* SWI */
        pf_at = 1;                                             /* (the OS returns with a branch) */
        if (swi_insn(i & 0xFFFFFF, next)) return 1;
        return npc != next;
    } else { undefined_insn(pc, i); return 1; }
    return 0;
}

#define ISTAT 4096
static struct { uint32_t a, why; uint64_t n; } istat[ISTAT];
static void note_interp(uint32_t a, uint32_t why)
{
    unsigned h = (a >> 2) % ISTAT, probes = 0;
    while (istat[h].n && (istat[h].a != a || istat[h].why != why))
        if (++probes == ISTAT) return;          /* table full: no more statistics */
        else h = (h + 1) % ISTAT;
    istat[h].a = a; istat[h].why = why; istat[h].n++;
}
/* RECOMP_ISTAT=1: what was interpreted, at exit */
void cpu_report_interp(void)
{
    if (getenv("RECOMP_ISTAT"))
        for (int k = 0; k < ISTAT; k++) if (istat[k].n)
            fprintf(stderr, "interp %08x %s %08x x%llu\n", istat[k].a, istat[k].why ? "smc-form" : "no-entry",
                    istat[k].why, (unsigned long long)istat[k].n);
}

void interp_step(void)
{
    uint32_t pc = npc;
    I_(pc);
    interp_count++;
    note_interp(pc, 0);
    uint32_t w;
    if (pc == pf_at && icount == pf_ic + 1) {      /* in sequence: as fetched */
        w = pf_w[0];
        pf_w[0] = pf_w[1];
    } else {                                       /* instruction fetches, not data reads */
        COV_NOFETCH(w = RD32(pc));
        COV_NOFETCH(pf_w[0] = RD32(pc + 4));
    }
    COV_NOFETCH(pf_w[1] = RD32(pc + 8));
    pf_at = pc + 4; pf_ic = icount;
    exec_word(pc, w);
}

int interp_exec(uint32_t a, uint32_t w)
{
    interp_count++;
    note_interp(a, w);
    return exec_word(a, w);
}
