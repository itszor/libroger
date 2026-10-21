/* The floating point emulator: the FPA's instructions (coprocessors 1 and
 * 2), which on an ARM2 or ARM3 are undefined instructions, taken from the
 * undefined instruction vector by RISC OS's FPEmulator module and carried
 * out in software - Acorn C's float and double arithmetic is these
 * instructions. As the module sits on the vector, a program's own branch
 * at &4 sees them first, an OS_ChangeEnvironment 1 handler never does
 * (cpu.c: undefined_insn).
 *
 * Eight registers of extended precision (long double), each result
 * rounded to the instruction's precision (single, double or extended) as
 * the FPA rounds it, in its rounding mode (nearest, +inf, -inf, zero);
 * the status register (RFS/WFS: the system ID for a software emulator,
 * the cumulative exception flags - no exception is trapped). The memory
 * formats are the FPA's: a single a word; a double two, the word with the
 * sign and exponent first; an extended three (sign and exponent, then the
 * mantissa with its explicit integer bit, high word first), also what
 * SFM/LFM store. Packed decimal (LDFP/STFP) isn't implemented. */
#include <fenv.h>
#include <math.h>
#include <string.h>
#include "internal.h"

static long double f[8];
static uint32_t fpsr = 0x81000000u;          /* system ID &81: the FPE (no hardware) */
#define FPSR_IVO 1u                           /* cumulative exception flags */
#define FPSR_DVZ 2u
#define FPSR_OFL 4u
#define FPSR_UFL 8u
#define FPSR_INX 16u

static const long double fconst[8] = {0.0L, 1.0L, 2.0L, 3.0L, 4.0L, 5.0L, 0.5L, 10.0L};

/* ---------------------------------------------------------------- rounding */
static const int fe_mode[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};

static long double to_prec(long double v, int prec)
{
    switch (prec) {
    case 0: return (float)v;
    case 1: return (double)v;
    default: return v;
    }
}

static void flags_of(long double v)
{
    if (isnan(v)) fpsr |= FPSR_IVO;
    else if (isinf(v)) fpsr |= FPSR_OFL;
}

/* ---------------------------------------------------------------- memory formats */
static long double load_ext(uint32_t a)
{
    uint32_t w0 = RD32(a), hi = RD32(a + 4), lo = RD32(a + 8);
    int e = (int)(w0 & 0x7FFF), neg = w0 >> 31;
    uint64_t m = (uint64_t)hi << 32 | lo;
    long double v;
    if (e == 0x7FFF) v = (m << 1) ? NAN : INFINITY;
    else if (!m) v = 0.0L;
    else v = ldexpl((long double)m, (e ? e : 1) - 16383 - 63);
    return neg ? -v : v;
}

static void store_ext(uint32_t a, long double v)
{
    uint32_t w0 = signbit(v) ? 0x80000000u : 0;
    uint64_t m = 0;
    v = fabsl(v);
    if (isnan(v)) { w0 |= 0x7FFF; m = 0xC000000000000000ull; }
    else if (isinf(v)) w0 |= 0x7FFF;
    else if (v != 0.0L) {
        int e;
        long double fr = frexpl(v, &e);              /* v = fr * 2^e, fr in [0.5, 1) */
        int be = e - 1 + 16383;
        if (be <= 0) { m = (uint64_t)ldexpl(fr, 64 + be - 1); be = 0; }   /* denormal */
        else m = (uint64_t)ldexpl(fr, 64);
        w0 |= (uint32_t)be;
    }
    WR32(a, w0); WR32(a + 4, (uint32_t)(m >> 32)); WR32(a + 8, (uint32_t)m);
}

static long double load(uint32_t a, int prec)
{
    if (prec == 0) {
        uint32_t w = RD32(a); float x;
        memcpy(&x, &w, 4);
        return x;
    }
    if (prec == 1) {
        uint64_t w = (uint64_t)RD32(a) << 32 | RD32(a + 4); double x;
        memcpy(&x, &w, 8);
        return x;
    }
    return load_ext(a);
}

static void store(uint32_t a, long double v, int prec)
{
    if (prec == 0) {
        float x = (float)v; uint32_t w;
        memcpy(&w, &x, 4);
        WR32(a, w);
    } else if (prec == 1) {
        double x = (double)v; uint64_t w;
        memcpy(&w, &x, 8);
        WR32(a, (uint32_t)(w >> 32)); WR32(a + 4, (uint32_t)w);
    } else store_ext(a, v);
}

/* ---------------------------------------------------------------- the classes */
/* LDF/STF (coprocessor 1), LFM/SFM (2): [Rn, #+-offset*4](!) or [Rn], #+-offset*4 */
static void data_transfer(uint32_t a, uint32_t w, int cp)
{
    uint32_t p = w >> 24 & 1, u = w >> 23 & 1, wb = w >> 21 & 1, l = w >> 20 & 1;
    uint32_t rn = w >> 16 & 15, fd = w >> 12 & 7, off = (w & 0xFF) * 4;
    uint32_t base = rn == 15 ? a + 8 : R[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t addr = p ? moved : base;
    if (cp == 1) {
        int prec = (int)((w >> 22 & 1) << 1 | (w >> 15 & 1));
        if (prec == 3) finish("FPA packed decimal (LDFP/STFP)");
        if (l) f[fd] = load(addr, prec);
        else store(addr, f[fd], prec);
    } else {                                          /* LFM/SFM: 1-4 registers from fd, wrapping */
        int n = (int)((w >> 15 & 1) | (w >> 22 & 1) << 1);
        if (!n) n = 4;
        for (int k = 0; k < n; k++) {
            uint32_t r = (fd + (uint32_t)k) & 7;
            if (l) f[r] = load_ext(addr + 12 * (uint32_t)k);
            else store_ext(addr + 12 * (uint32_t)k, f[r]);
        }
    }
    if ((wb || !p) && rn != 15) R[rn] = moved;
}

/* the data operations: dyadic Fd := Fn op Fm, monadic Fd := op Fm (bit 15) */
static void data_op(uint32_t w)
{
    uint32_t op = w >> 20 & 15, fd = w >> 12 & 7, fn = w >> 16 & 7;
    int prec = (int)((w >> 19 & 1) << 1 | (w >> 7 & 1)), mode = (int)(w >> 5 & 3);
    long double m = w & 8 ? fconst[w & 7] : f[w & 7], n = f[fn], r;
    int old = fegetround();
    if (mode) fesetround(fe_mode[mode]);
    if (w >> 15 & 1) {
        switch (op) {
        case 0: case 15: r = m; break;                         /* MVF, NRM */
        case 1: r = -m; break;                                 /* MNF */
        case 2: r = fabsl(m); break;                           /* ABS */
        case 3: case 14: r = nearbyintl(m); break;             /* RND, URD */
        case 4: r = sqrtl(m); break;                           /* SQT */
        case 5: r = log10l(m); break;                          /* LOG */
        case 6: r = logl(m); break;                            /* LGN */
        case 7: r = expl(m); break;                            /* EXP */
        case 8: r = sinl(m); break;                            /* SIN */
        case 9: r = cosl(m); break;                            /* COS */
        case 10: r = tanl(m); break;                           /* TAN */
        case 11: r = asinl(m); break;                          /* ASN */
        case 12: r = acosl(m); break;                          /* ACS */
        default: r = atanl(m); break;                          /* ATN */
        }
    } else {
        switch (op) {
        case 0: r = n + m; break;                              /* ADF */
        case 1: r = n * m; break;                              /* MUF */
        case 2: r = n - m; break;                              /* SUF */
        case 3: r = m - n; break;                              /* RSF */
        case 4: if (m == 0.0L) fpsr |= FPSR_DVZ; r = n / m; break;   /* DVF */
        case 5: if (n == 0.0L) fpsr |= FPSR_DVZ; r = m / n; break;   /* RDF */
        case 6: r = powl(n, m); break;                         /* POW */
        case 7: r = powl(m, n); break;                         /* RPW */
        case 8: r = remainderl(n, m); break;                   /* RMF */
        case 9: r = (float)(n * m); prec = 0; break;           /* FML: single precision */
        case 10: r = (float)(n / m); prec = 0; break;          /* FDV */
        case 11: r = (float)(m / n); prec = 0; break;          /* FRD */
        case 12: r = atan2l(n, m); break;                      /* POL */
        default: r = NAN; break;
        }
    }
    r = to_prec(r, prec);
    if (mode) fesetround(old);
    flags_of(r);
    f[fd] = r;
}

/* the register transfers: FLT, FIX, WFS, RFS, WFC, RFC; CMF, CNF, CMFE,
 * CNFE (Fn with Fm, or -Fm), which set the ARM's flags */
static int reg_transfer(uint32_t a, uint32_t w)
{
    uint32_t op = w >> 21 & 7, l = w >> 20 & 1, rd = w >> 12 & 15, fn = w >> 16 & 7;
    int prec = (int)((w >> 19 & 1) << 1 | (w >> 7 & 1)), mode = (int)(w >> 5 & 3);
    long double m = w & 8 ? fconst[w & 7] : f[w & 7];
    (void)a;
    if (l && rd == 15 && op >= 4) {
        long double x = f[fn], y = op & 1 ? -m : m;
        if (isnan(x) || isnan(y)) { NF = 0; ZF = 0; CF = 1; VF = 1; fpsr |= op >= 6 ? FPSR_IVO : 0; }
        else if (x == y) { NF = 0; ZF = 1; CF = 1; VF = 0; }
        else if (x < y) { NF = 1; ZF = 0; CF = 0; VF = 0; }
        else { NF = 0; ZF = 0; CF = 1; VF = 0; }
        return 1;
    }
    switch (op << 1 | l) {
    case 0: {                                                  /* FLT Fn, Rd */
        int old = fegetround();
        if (mode) fesetround(fe_mode[mode]);
        f[fn] = to_prec((long double)(int32_t)R[rd], prec);
        if (mode) fesetround(old);
        return 1;
    }
    case 1: {                                                  /* FIX Rd, Fm */
        long double r;
        switch (mode) {
        case 0: r = nearbyintl(m); break;   /* (the current mode is nearest) */
        case 1: r = ceill(m); break;
        case 2: r = floorl(m); break;
        default: r = truncl(m); break;
        }
        int32_t v;
        if (isnan(r)) { v = 0x7FFFFFFF; fpsr |= FPSR_IVO; }
        else if (r > 2147483647.0L) { v = 0x7FFFFFFF; fpsr |= FPSR_IVO; }
        else if (r < -2147483648.0L) { v = (int32_t)0x80000000u; fpsr |= FPSR_IVO; }
        else v = (int32_t)r;
        if (r != m) fpsr |= FPSR_INX;
        if (rd != 15) R[rd] = (uint32_t)v;
        return 1;
    }
    case 2: fpsr = (fpsr & 0xFF000000u) | (R[rd] & 0x00FFFFFFu); return 1;   /* WFS */
    case 3: if (rd != 15) R[rd] = fpsr; return 1;                             /* RFS */
    case 4: return 1;                                                         /* WFC */
    case 5: if (rd != 15) R[rd] = 0; return 1;                                /* RFC */
    }
    return 0;
}

/* An undefined instruction: if it's the FPA's, carry it out and go on to
 * the next (1); otherwise 0 */
int fpa_insn(uint32_t a, uint32_t w)
{
    uint32_t cp = w >> 8 & 15, t = w >> 24 & 15;
    if (cp != 1 && cp != 2) return 0;
    if (t == 12 || t == 13) data_transfer(a, w, (int)cp);     /* LDC/STC: LDF/STF, LFM/SFM */
    else if (t == 14 && cp == 1) {
        if (w & 0x10) { if (!reg_transfer(a, w)) return 0; }  /* MRC/MCR */
        else data_op(w);                                       /* CDP */
    } else return 0;
    npc = (a + 4) & 0x3FFFFFC;
    return 1;
}
