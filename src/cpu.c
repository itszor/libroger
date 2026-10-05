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

/* ---------------------------------------------------------------- images
 * The program image (where &8000-based code is dispatched; replaced when a
 * chain-loaded program takes over) plus any others, e.g. modules in the
 * RMA, dispatched wherever their address range is. */
struct loaded { const struct roger_image *img; fn_t *tab; };
static struct loaded prog, extra[16];
static int nextra;

static fn_t *build_tab(const struct roger_image *img)
{
    fn_t *tab = calloc((img->end - img->base) / 4 + 1, sizeof(fn_t));
    for (unsigned k = 0; k < img->nentries; k++) tab[(img->entries[k].a - img->base) >> 2] = img->entries[k].f;
    return tab;
}

static fn_t *tab_for(const struct roger_image *img)
{
    static struct loaded cache[16];
    for (int k = 0; k < 16; k++) {
        if (cache[k].img == img) return cache[k].tab;
        if (!cache[k].img) { cache[k].img = img; return cache[k].tab = build_tab(img); }
    }
    return build_tab(img);
}

void roger_set_image(const struct roger_image *img)
{
    prog.img = img;
    prog.tab = tab_for(img);
    img_base = img->base; img_end = img->end; codemap = img->codemap;
    (void)COV_TAG(img->cov_tag ? img->cov_tag : 'P');
}
const struct roger_image *roger_current_image(void) { return prog.img; }

/* Would dispatching a reach translated code? (relocations applied) */
int cpu_translated(uint32_t a)
{
    if (rg->relocations)
        for (const struct roger_relocation *r = rg->relocations; r->image; r++)
            if (prog.img == r->image && a >= r->to && a < r->to + r->len) a -= r->to - r->from;
    if (a >= img_base && a < img_end) return prog.tab[(a - img_base) >> 2] != 0;
    for (int k = 0; k < nextra; k++)
        if (a >= extra[k].img->base && a < extra[k].img->end) return extra[k].tab[(a - extra[k].img->base) >> 2] != 0;
    return 0;
}

void cpu_add_image(const struct roger_image *img)
{
    if (nextra < 16) { extra[nextra].img = img; extra[nextra].tab = tab_for(img); nextra++; }
}

/* Programs that may be loaded at their image's base: OS_File loaded one;
 * it takes over when control next arrives at its base. */
static const struct roger_image *pending;
void cpu_programs_loaded(const char *leaf, uint32_t addr)
{
    if (!rg->programs) return;
    for (const struct roger_program *p = rg->programs; p->leaf; p++)
        if (!strcasecmp(p->leaf, leaf) && p->image->base == addr) pending = p->image;
}

/* ---------------------------------------------------------------- dispatch */
static int seen(uint32_t *tab, int *n, int max, uint32_t a)
{
    for (int k = 0; k < *n; k++) if (tab[k] == a) return 1;
    if (*n < max) tab[(*n)++] = a;
    return 0;
}
int cpu_seen_gap(uint32_t a) { static uint32_t s[4096]; static int n; return seen(s, &n, 4096, a); }

void cpu_log_gap(uint32_t a)
{
    FILE *f = fopen("untranslated.log", "a");
    if (f) {
        fprintf(f, "%llu: branch to untranslated address %08x (LR=%08x, from pc=%08x)\n",
                (unsigned long long)icount, a, R[14], g_cur_pc);
        fclose(f);
    }
    fprintf(stderr, "branch to untranslated address %08x (icount %llu, LR=%08x, from pc=%08x) - see untranslated.log\n",
            a, (unsigned long long)icount, R[14], g_cur_pc);
}

void call_addr(uint32_t a)
{
    if (a == RETURN_TO_OS) finish("returned to OS");
    if (rg->call_addr && rg->call_addr(a)) return;
    if (rg->relocations)
        for (const struct roger_relocation *r = rg->relocations; r->image; r++)
            if (prog.img == r->image && a >= r->to && a < r->to + r->len) {
                pc_bias = r->to - r->from;
                a -= pc_bias;
                npc = a;
            }
    if (pending && a == pending->base) {
        fprintf(stderr, "chain-load: program at %08x replaced (icount %llu)\n", a, (unsigned long long)icount);
        roger_set_image(pending);
        pending = NULL;
        pc_bias = 0;
    }
    fn_t f = 0;
    if (!interp_only) {
        if (a >= img_base && a < img_end) f = prog.tab[(a - img_base) >> 2];
        else for (int k = 0; k < nextra; k++)
            if (a >= extra[k].img->base && a < extra[k].img->end) { f = extra[k].tab[(a - extra[k].img->base) >> 2]; break; }
    }
    if (f) { f(a); return; }
    if (a == HANDLER_RET) return;
    if (rg->interpreted)
        for (const uint32_t *g = rg->interpreted; g[1]; g += 2)
            if (a >= g[0] && a < g[1]) { interp_step(); return; }   /* a known gap */
    if (rg->interpret_gaps) { interp_step(); return; }
    for (int k = 0; k < nextra; k++)
        if (a >= extra[k].img->base && a < extra[k].img->end) {
            /* module code should be fully translated: report the gap and end
             * this call rather than the whole run */
            if (!cpu_seen_gap(a)) cpu_log_gap(a);
            npc = HANDLER_RET;
            return;
        }
    if (in_handler && !rg_trace_gaps) {
        /* an interrupt handler the runtime called landed somewhere it can't
         * follow: end that call (the next interrupt tries again) */
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
        call_addr(npc);
    }
}
void roger_run_until(uint32_t stop) { run_until(stop); }

/* Run code at addr in processor mode `mode` with R14 = r14 until it returns
 * to HANDLER_RET (interrupt handlers, voice generators, module entries).
 * No interrupts are taken meanwhile. */
void call_code(uint32_t addr, uint32_t mode, uint32_t r14)
{
    uint32_t snpc = npc, smode = MODEBITS;
    int sh = in_handler;
    in_handler = 1;
    set_psr_mode(mode);
    R[14] = r14;
    npc = addr;
    COV_ASYNC_ENTER();
    uint64_t i0 = icount;
    while (npc != HANDLER_RET) {
        if (npc == RETURN_TO_OS) finish("returned to OS");
        if (icount - i0 > 50000000) {
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
void roger_call(uint32_t addr, uint32_t mode, uint32_t r14) { call_code(addr, mode, r14); }
int roger_in_handler(void) { return in_handler; }

void bad_entry(uint32_t fn, uint32_t e)
{
    fprintf(stderr, "bad entry %08x into F_%x\n", e, fn);
    finish("bad entry");
}
void undefined_insn(uint32_t a, uint32_t w)
{
    fprintf(stderr, "undefined/coproc instruction %08x at %08x\n", w, a);
    finish("undefined instruction");
}
void smc_write(uint32_t a)
{
    static uint32_t s[64]; static int n;
    if (seen(s, &n, 64, a)) return;
    smc_count++;
    fprintf(stderr, "note: store to compiled code at %08x (icount %llu)\n", a, (unsigned long long)icount);
}

/* ---------------------------------------------------------------- diagnostics
 * PC histogram (after the first 700000 instructions), printed at exit:
 * where the program spends its time when it seems stuck. Open-addressed
 * with a bounded probe, so a PC that finds no slot is just not counted. */
#define PCHIST 65536
#define PCHIST_PROBE 32
static struct { uint32_t a; uint64_t n; } pchist[PCHIST];
uint32_t g_cur_pc;
void dbg_hook(uint32_t a)
{
    g_cur_pc = a;
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
    if (op <= 1 && (i & 0x0FC000F0) == 0x00000090) {          /* MUL/MLA */
        int rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
        uint32_t v = R[rm] * R[rs];
        if (i & (1 << 21)) v += R[rn];
        R[rd] = v;
        if (i & (1 << 20)) SETNZ(v);
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
        if (l) {
            if (w && rn != 15) R[rn] = wbv;
            for (int k = 0; k < 16; k++) if (i & (1 << k)) {
                uint32_t v2 = RD32(a);
                if (k == 15) { setpc_i(v2, s); jumped = 1; } else R[k] = v2;
                a += 4;
            }
        } else {
            for (int k = 0; k < 16; k++) if (i & (1 << k)) {
                WR32(a, k == 15 ? ((pc + 12) & 0x3FFFFFC) | PSR() : R[k]);
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
        swi(i & 0xFFFFFF, next);
        return npc != next;
    } else undefined_insn(pc, i);
    return 0;
}

#define ISTAT 4096
static struct { uint32_t a, why; uint64_t n; } istat[ISTAT];
static void note_interp(uint32_t a, uint32_t why)
{
    unsigned h = (a >> 2) % ISTAT;
    while (istat[h].n && (istat[h].a != a || istat[h].why != why)) h = (h + 1) % ISTAT;
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
    I(pc);
    interp_count++;
    note_interp(pc, 0);
    uint32_t w;
    COV_NOFETCH(w = RD32(pc));     /* instruction fetch, not a data read */
    exec_word(pc, w);
}

int interp_exec(uint32_t a, uint32_t w)
{
    interp_count++;
    note_interp(a, w);
    return exec_word(a, w);
}
