/* The Shared C Library (SharedCLibrary, SWIs &80680-&80684), on the host's.
 *
 * A C program linked with the library's stubs (o.stubs, AIF) starts with
 * SharedCLibrary_LibInitAPCS_R: r0 points to a list of chunk descriptors
 * {id, vector start, vector end, static data start, static data end}, id
 * -1 ending it. The library fills each chunk's entry vector - one word per
 * function, in the order the library defines - with branches to itself and
 * initialises the static data the client keeps for it (the C library's
 * starts with errno, the 16 FILEs of __iob and the __ctype table, which the
 * client's getc/putc/isxxx macros read directly). Chunk 1 is the kernel
 * (_kernel_*), chunk 2 the C library proper.
 *
 * Here the vector words are marked self-modified, so translated code
 * leaves through the dispatcher at them, and call_addr() routes them to the
 * functions below, written on the host's C library: arguments in R0-R3 then
 * on the stack (APCS-R), results in R0 (R0/R1 for the divisions), return to
 * R14 restoring the PSR. Interfaces are as the library's headers (and RISC
 * OS Open's RISC_OSLib sources: entry order, data layout) document them; no
 * code comes from it. Behaviour is C89's, as the 1990s library had it:
 * long is 32 bits, doubles are FPA doubles (high word first), stdio on the
 * OS's files (OS_Find/GBPB/BGet/BPut/Args, so paths and the save directory
 * are the runtime's), the console through the VDU.
 *
 * Not provided: floating point arithmetic in the client (no FP emulator),
 * stack extension (one large root chunk instead: overflow ends the run),
 * signals raised asynchronously (Escape), modules linked with the library
 * (LibInitModule), locales other than "C". A function not implemented here
 * ends the run naming it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include "internal.h"

#define LIB_VERSION   5                   /* LibInit's R6: the library's interface version */
#define ROOT_STACK    0x10000u            /* the root stack chunk (no extension) */
#define SC_SLOFFSET   560                 /* sl = chunk base + this (APCS-R stack chunk) */
#define SL_LIB_OFFSET (-540)              /* [sl-540], [sl-536]: static data relocations */

/* ------------------------------------------------------------ entry tables */
/* The entries, in the library's order (a client's vector may be shorter: its
 * library version had fewer) */
#define KERNEL_ENTRIES(X) \
    X(_kernel_init, "_kernel_init") X(_kernel_exit, "_kernel_exit") \
    X(_kernel_setreturncode, "_kernel_setreturncode") \
    X(_kernel_exittraphandler, "_kernel_exittraphandler") \
    X(_kernel_unwind, "_kernel_unwind") X(_kernel_procname, "_kernel_procname") \
    X(_kernel_language, "_kernel_language") \
    X(_kernel_command_string, "_kernel_command_string") X(_kernel_hostos, "_kernel_hostos") \
    X(_kernel_swi, "_kernel_swi") X(_kernel_osbyte, "_kernel_osbyte") \
    X(_kernel_osrdch, "_kernel_osrdch") X(_kernel_oswrch, "_kernel_oswrch") \
    X(_kernel_osbget, "_kernel_osbget") X(_kernel_osbput, "_kernel_osbput") \
    X(_kernel_osgbpb, "_kernel_osgbpb") X(_kernel_osword, "_kernel_osword") \
    X(_kernel_osfind, "_kernel_osfind") X(_kernel_osfile, "_kernel_osfile") \
    X(_kernel_osargs, "_kernel_osargs") X(_kernel_oscli, "_kernel_oscli") \
    X(_kernel_last_oserror, "_kernel_last_oserror") X(_kernel_system, "_kernel_system") \
    X(_kernel_getenv, "_kernel_getenv") X(_kernel_setenv, "_kernel_setenv") \
    X(_kernel_register_allocs, "_kernel_register_allocs") X(_kernel_alloc, "_kernel_alloc") \
    X(_kernel_stkovf_split_0frame, "_kernel_stkovf_split_0frame") \
    X(_kernel_stkovf_split, "_kernel_stkovf_split") \
    X(_kernel_stkovf_copyargs, "_kernel_stkovf_copyargs") \
    X(_kernel_stkovf_copy0args, "_kernel_stkovf_copy0args") X(_kernel_udiv, "_kernel_udiv") \
    X(_kernel_urem, "_kernel_urem") X(_kernel_udiv10, "_kernel_udiv10") \
    X(_kernel_sdiv, "_kernel_sdiv") X(_kernel_srem, "_kernel_srem") \
    X(_kernel_sdiv10, "_kernel_sdiv10") X(_kernel_fpavailable, "_kernel_fpavailable") \
    X(_kernel_moduleinit, "_kernel_moduleinit") X(_kernel_irqs_on, "_kernel_irqs_on") \
    X(_kernel_irqs_off, "_kernel_irqs_off") \
    X(_kernel_irqs_disabled, "_kernel_irqs_disabled") \
    X(_kernel_entermodule, "_kernel_entermodule") \
    X(_kernel_escape_seen, "_kernel_escape_seen") \
    X(_kernel_current_stack_chunk, "_kernel_current_stack_chunk") \
    X(_kernel_swi_c, "_kernel_swi_c") \
    X(_kernel_register_slotextend, "_kernel_register_slotextend") \
    X(_kernel_raise_error, "_kernel_raise_error")
#define CLIB_ENTRIES(X) \
    X(TrapHandler, "TrapHandler") X(UncaughtTrapHandler, "UncaughtTrapHandler") \
    X(EventHandler, "EventHandler") X(UnhandledEventHandler, "UnhandledEventHandler") \
    X(x_stack_overflow, "x$stack_overflow") X(x_stack_overflow_1, "x$stack_overflow_1") \
    X(x_udivide, "x$udivide") X(x_uremainder, "x$uremainder") X(x_divide, "x$divide") \
    X(x_divtest, "x$divtest") X(x_remainder, "x$remainder") X(x_multiply, "x$multiply") \
    X(_rd1chk, "_rd1chk") X(_rd2chk, "_rd2chk") X(_rd4chk, "_rd4chk") X(_wr1chk, "_wr1chk") \
    X(_wr2chk, "_wr2chk") X(_wr4chk, "_wr4chk") X(_main, "_main") X(_exit, "_exit") \
    X(_clib_initialise, "_clib_initialise") X(_backtrace, "_backtrace") X(_count, "_count") \
    X(_count1, "_count1") X(_stfp, "_stfp") X(_ldfp, "_ldfp") X(_printf, "_printf") \
    X(_fprintf, "_fprintf") X(_sprintf, "_sprintf") X(clock, "clock") \
    X(difftime, "difftime") X(mktime, "mktime") X(time, "time") X(asctime, "asctime") \
    X(ctime, "ctime") X(gmtime, "gmtime") X(localtime, "localtime") X(strftime, "strftime") \
    X(memcpy, "memcpy") X(memmove, "memmove") X(strcpy, "strcpy") X(strncpy, "strncpy") \
    X(strcat, "strcat") X(strncat, "strncat") X(memcmp, "memcmp") X(strcmp, "strcmp") \
    X(strncmp, "strncmp") X(memchr, "memchr") X(strchr, "strchr") X(strcspn, "strcspn") \
    X(strpbrk, "strpbrk") X(strrchr, "strrchr") X(strspn, "strspn") X(strstr, "strstr") \
    X(strtok, "strtok") X(memset, "memset") X(strerror, "strerror") X(strlen, "strlen") \
    X(atof, "atof") X(atoi, "atoi") X(atol, "atol") X(strtod, "strtod") X(strtol, "strtol") \
    X(strtoul, "strtoul") X(rand, "rand") X(srand, "srand") X(calloc, "calloc") \
    X(free, "free") X(malloc, "malloc") X(realloc, "realloc") X(abort, "abort") \
    X(atexit, "atexit") X(exit, "exit") X(getenv, "getenv") X(system, "system") \
    X(bsearch, "bsearch") X(qsort, "qsort") X(abs, "abs") X(div, "div") X(labs, "labs") \
    X(ldiv, "ldiv") X(remove, "remove") X(rename, "rename") X(tmpfile, "tmpfile") \
    X(__old_tmpnam, "__old_tmpnam") X(fclose, "fclose") X(fflush, "fflush") \
    X(fopen, "fopen") X(freopen, "freopen") X(setbuf, "setbuf") X(setvbuf, "setvbuf") \
    X(printf, "printf") X(fprintf, "fprintf") X(sprintf, "sprintf") X(scanf, "scanf") \
    X(fscanf, "fscanf") X(sscanf, "sscanf") X(vprintf, "vprintf") X(vfprintf, "vfprintf") \
    X(vsprintf, "vsprintf") X(_vfprintf, "_vfprintf") X(fgetc, "fgetc") X(fgets, "fgets") \
    X(fputc, "fputc") X(fputs, "fputs") X(__filbuf, "__filbuf") X(getc, "getc") \
    X(getchar, "getchar") X(gets, "gets") X(__flsbuf, "__flsbuf") X(putc, "putc") \
    X(putchar, "putchar") X(puts, "puts") X(ungetc, "ungetc") X(fread, "fread") \
    X(fwrite, "fwrite") X(fgetpos, "fgetpos") X(fseek, "fseek") X(fsetpos, "fsetpos") \
    X(ftell, "ftell") X(rewind, "rewind") X(clearerr, "clearerr") X(feof, "feof") \
    X(ferror, "ferror") X(perror, "perror") \
    X(__ignore_signal_handler, "__ignore_signal_handler") \
    X(__default_signal_handler, "__default_signal_handler") \
    X(__error_signal_marker, "__error_signal_marker") X(signal, "signal") X(raise, "raise") \
    X(setjmp, "setjmp") X(longjmp, "longjmp") X(acos, "acos") X(asin, "asin") \
    X(atan, "atan") X(atan2, "atan2") X(cos, "cos") X(sin, "sin") X(tan, "tan") \
    X(cosh, "cosh") X(sinh, "sinh") X(tanh, "tanh") X(exp, "exp") X(frexp, "frexp") \
    X(ldexp, "ldexp") X(log, "log") X(log10, "log10") X(modf, "modf") X(pow, "pow") \
    X(sqrt, "sqrt") X(ceil, "ceil") X(fabs, "fabs") X(floor, "floor") X(fmod, "fmod") \
    X(setlocale, "setlocale") X(isalnum, "isalnum") X(isalpha, "isalpha") \
    X(iscntrl, "iscntrl") X(isdigit, "isdigit") X(isgraph, "isgraph") X(islower, "islower") \
    X(isprint, "isprint") X(ispunct, "ispunct") X(isspace, "isspace") X(isupper, "isupper") \
    X(isxdigit, "isxdigit") X(tolower, "tolower") X(toupper, "toupper") \
    X(__assert, "__assert") X(_memcpy, "_memcpy") X(_memset, "_memset") \
    X(localeconv, "localeconv") X(mblen, "mblen") X(mbtowc, "mbtowc") X(wctomb, "wctomb") \
    X(mbstowcs, "mbstowcs") X(wcstombs, "wcstombs") X(strxfrm, "strxfrm") \
    X(strcoll, "strcoll") X(_clib_finalisemodule, "_clib_finalisemodule") \
    X(_clib_version, "_clib_version") X(_CLib_Finalise, "_CLib_Finalise") \
    X(tmpnam, "tmpnam") X(_swi, "_swi") X(_swix, "_swix")
#define X(i, s) K_##i,
enum { KERNEL_ENTRIES(X) NKERNEL };
#undef X
#define X(i, s) C_##i,
enum { CLIB_ENTRIES(X) NCLIB };
#undef X
#define X(i, s) s,
static const char *const kernel_names[] = {KERNEL_ENTRIES(X)};
static const char *const clib_names[] = {CLIB_ENTRIES(X)};
#undef X

/* the client's vectors and static data */
static struct { uint32_t vec, n, data, dlen; } chunk[3];   /* [1] kernel, [2] C library */
static int inited;

/* the C library's static data: offsets of what clients use directly */
#define D_ERRNO   0
#define D_IOB     4                       /* 16 FILEs of 40 bytes */
#define D_CTYPE   656                     /* 256 bytes; ctype_eof (the EOF entry) just before */
#define D_HUGEVAL 912
#define NFILES    16
#define FILE_SIZE 40
/* a FILE: ptr, icnt, ocnt, flag, base, file (OS handle), unused, bufsiz,
 * signature, extrap. icnt and ocnt stay 0, so the getc/putc macros always
 * call __filbuf/__flsbuf. */
#define F_ICNT 4
#define F_OCNT 8
#define F_FLAG 12
#define F_FILE 20
#define IOREAD 0x01
#define IOWRITE 0x02
#define IOEOF  0x40
#define IOERR  0x80
#define IONBF  0x400
#define SIGABRT 1
#define SIGFPE  2
#define NSIG    12
/* the kernel's static data: its error buffer (errorNumber, errorString) */
#define K_ERROR   240

static uint32_t heap_lo, heap_hi;         /* the C heap, between the root stack and the workspace end */
static uint32_t stack_base, stack_top;

/* ---------------------------------------------------------- the CPU side */
static inline char *P(uint32_t a) { return (char *)MEM + (a & 0x3FFFFFF); }

/* The registers at the call: the arguments (R0-R3, then the stack at R13). */
static uint32_t A[4], SP;

static void fail(const char *what, const char *name)
{
    fprintf(stderr, "SharedCLibrary: %s %s (called from %08x)\n", what, name, R[14] & 0x3FFFFFC);
    ev("clib_unimplemented", "s", name, (int)strlen(name));
    finish("C library function not implemented");
}

/* Call client code (a function pointer the program passed) with up to four
 * arguments; returns its R0. Registers, flags and the arguments of the
 * call being served are as they were after. */
static uint32_t call_client(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t save[16], f[4] = {NF, ZF, CF, VF}, snpc = npc, sa[4], ssp = SP;
    memcpy(save, R, sizeof save);
    memcpy(sa, A, sizeof sa);
    R[0] = a0; R[1] = a1; R[2] = a2; R[3] = a3;
    roger_call_program(fn & 0x3FFFFFC, HANDLER_RET);
    uint32_t r0 = R[0];
    memcpy(R, save, sizeof save);
    memcpy(A, sa, sizeof sa);
    SP = ssp;
    NF = f[0]; ZF = f[1]; CF = f[2]; VF = f[3];
    npc = snpc;
    return r0;
}

/* A SWI (X form unless nonx) with R[] as set; returns the error block's
 * address, or 0. */
static uint32_t do_swi(uint32_t n, int nonx)
{
    uint32_t snpc = npc;
    roger_swi(nonx ? n & ~0x20000u : n | 0x20000u);
    npc = snpc;
    if (VF) {                             /* keep a copy for _kernel_last_oserror */
        if (chunk[1].data && R[0] && R[0] < MEMSIZE - 256) {
            memcpy(P(chunk[1].data + K_ERROR), P(R[0]), 252);
            MEM[chunk[1].data + K_ERROR + 251] = 0;
        }
        return R[0];
    }
    return 0;
}

/* Variadic arguments: from argument k on (A[], then the stack), or from a
 * va_list (a pointer to the words). */
struct args { int k; uint32_t mem; };
static uint32_t arg_word(struct args *a)
{
    if (a->mem) { uint32_t v = RD32(a->mem); a->mem += 4; return v; }
    uint32_t v = a->k < 4 ? A[a->k] : RD32(SP + 4 * (a->k - 4));
    a->k++;
    return v;
}
static double fpa_double(uint32_t hi, uint32_t lo)
{
    uint64_t b = (uint64_t)hi << 32 | lo;
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static void put_double(uint32_t a, double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    WR32(a, (uint32_t)(b >> 32));
    WR32(a + 4, (uint32_t)b);
}
static void ret_double(double d)          /* a double result: R0 (high word), R1 */
{
    uint64_t b;
    memcpy(&b, &d, 8);
    R[0] = (uint32_t)(b >> 32);
    R[1] = (uint32_t)b;
}
static double arg_double(struct args *a) { uint32_t hi = arg_word(a), lo = arg_word(a); return fpa_double(hi, lo); }

/* a pointer into the same string: the emulated address */
static uint32_t at(uint32_t base, const char *p) { return p ? base + (uint32_t)(p - P(base)) : 0; }
/* copy a string, at most n characters (pad: with zeros to n, as strncpy;
 * otherwise the terminator is copied if reached); returns where it stopped */
static uint32_t copy_str(uint32_t d, uint32_t s, uint32_t n, int pad)
{
    uint32_t i = 0;
    for (; i < n; i++) {
        uint8_t c = MEM[(s + i) & 0x3FFFFFF];
        MEM[(d + i) & 0x3FFFFFF] = c;
        if (!c) { if (pad) while (++i < n) MEM[(d + i) & 0x3FFFFFF] = 0; return d + i; }
    }
    return d + i;
}
static uint32_t sign(int c) { return c < 0 ? (uint32_t)-1 : c > 0; }

/* ------------------------------------------------------------- the heap */
/* First fit over [heap_lo, heap_hi); blocks are kept host-side, sorted. */
static struct blk { uint32_t a, n; int used; } *blk;
static int nblk, cblk;

static void heap_init(void)
{
    free(blk);
    cblk = 64; nblk = 1;
    blk = malloc(cblk * sizeof *blk);
    blk[0] = (struct blk){heap_lo, heap_hi - heap_lo, 0};
}
static uint32_t c_malloc(uint32_t n)
{
    n = (n + 7) & ~7u;
    if (!n) n = 8;
    for (int k = 0; k < nblk; k++)
        if (!blk[k].used && blk[k].n >= n) {
            if (blk[k].n > n) {
                if (nblk == cblk) blk = realloc(blk, (cblk *= 2) * sizeof *blk);
                memmove(blk + k + 2, blk + k + 1, (nblk - k - 1) * sizeof *blk);
                blk[k + 1] = (struct blk){blk[k].a + n, blk[k].n - n, 0};
                blk[k].n = n;
                nblk++;
            }
            blk[k].used = 1;
            return blk[k].a;
        }
    ev("clib_malloc_failed", "i", n);
    return 0;
}
static int blk_find(uint32_t a)
{
    for (int k = 0; k < nblk; k++) if (blk[k].a == a && blk[k].used) return k;
    return -1;
}
static void c_free(uint32_t a)
{
    int k = blk_find(a);
    if (k < 0) return;
    blk[k].used = 0;
    if (k + 1 < nblk && !blk[k + 1].used) {
        blk[k].n += blk[k + 1].n;
        memmove(blk + k + 1, blk + k + 2, (nblk - k - 2) * sizeof *blk);
        nblk--;
    }
    if (k > 0 && !blk[k - 1].used) {
        blk[k - 1].n += blk[k].n;
        memmove(blk + k, blk + k + 1, (nblk - k - 1) * sizeof *blk);
        nblk--;
    }
}
static uint32_t c_realloc(uint32_t a, uint32_t n)
{
    if (!a) return c_malloc(n);
    int k = blk_find(a);
    if (k < 0) return 0;
    if (!n) { c_free(a); return 0; }
    uint32_t old = blk[k].n;
    if (((n + 7) & ~7u) <= old) return a;
    uint32_t b = c_malloc(n);
    if (b) { memmove(P(b), P(a), old); c_free(a); }
    return b;
}

/* ---------------------------------------------------------------- stdio */
static struct { int open, ungot, line_len, line_pos; char line[260]; } fs[NFILES];

static uint32_t file_addr(int k) { return chunk[2].data + D_IOB + FILE_SIZE * k; }
static int file_index(uint32_t f)
{
    uint32_t b = chunk[2].data + D_IOB;
    if (f < b || f >= b + NFILES * FILE_SIZE || (f - b) % FILE_SIZE) return -1;
    return (f - b) / FILE_SIZE;
}
static void set_flag(uint32_t f, uint32_t set, uint32_t clr) { WR32(f + F_FLAG, (RD32(f + F_FLAG) | set) & ~clr); }

static void console_put(int c)
{
    if (c == '\n') do_swi(3, 0);                    /* OS_NewLine */
    else { R[0] = c & 0xFF; do_swi(0, 0); }         /* OS_WriteC */
}

/* stdin: a line at a time, read with OS_ReadC and echoed (as OS_ReadLine) */
static int console_get(void)
{
    if (fs[0].line_pos >= fs[0].line_len) {
        int n = 0;
        for (;;) {
            if (do_swi(4, 0) || CF) return -1;      /* OS_ReadC; C: Escape */
            int c = R[0] & 0xFF;
            if (c == 13 || c == 10) { console_put('\n'); break; }
            if (c == 8 || c == 127) { if (n) { n--; console_put(127); } continue; }
            if (c < 32 || n >= 255) continue;
            fs[0].line[n++] = c;
            console_put(c);
        }
        fs[0].line[n++] = '\n';
        fs[0].line_len = n;
        fs[0].line_pos = 0;
    }
    return (unsigned char)fs[0].line[fs[0].line_pos++];
}

static int file_getc(uint32_t f)
{
    int k = file_index(f);
    if (k < 0 || !fs[k].open) return -1;
    WR32(f + F_ICNT, 0);
    if (fs[k].ungot >= 0) { int c = fs[k].ungot; fs[k].ungot = -1; return c; }
    int c;
    if (k == 0) c = console_get();
    else {
        R[1] = RD32(f + F_FILE);
        if (do_swi(0x0A, 0)) { set_flag(f, IOERR, 0); return -1; }     /* OS_BGet */
        c = CF ? -1 : (int)(R[0] & 0xFF);
    }
    if (c < 0) set_flag(f, IOEOF, 0);
    return c;
}
static int file_putc(int c, uint32_t f)
{
    int k = file_index(f);
    if (k < 0 || !fs[k].open) return -1;
    WR32(f + F_OCNT, 0);
    if (k == 1 || k == 2) { console_put(c); return c & 0xFF; }
    R[0] = c & 0xFF; R[1] = RD32(f + F_FILE);
    if (do_swi(0x0B, 0)) { set_flag(f, IOERR, 0); return -1; }         /* OS_BPut */
    return c & 0xFF;
}
static void file_puts(uint32_t f, const char *s, int n)
{
    for (int i = 0; i < n; i++) file_putc((unsigned char)s[i], f);
}

static uint32_t c_fopen(uint32_t name, uint32_t mode)
{
    const char *m = P(mode);
    int k;
    for (k = 3; k < NFILES && fs[k].open; k++) ;
    if (k == NFILES) return 0;
    int plus = strchr(m, '+') != 0;
    /* OS_Find: &40 read, &80 create, &C0 update; +8: an error for a directory */
    R[0] = (m[0] == 'r' ? (plus ? 0xC0 : 0x40) : m[0] == 'w' ? 0x80 : 0xC0) | 8;
    R[1] = name;
    if (do_swi(0x0D, 0) || !R[0]) {
        if (m[0] != 'a') return 0;
        R[0] = 0x80; R[1] = name;                                       /* "a": create it */
        if (do_swi(0x0D, 0) || !R[0]) return 0;
    }
    uint32_t h = R[0], f = file_addr(k);
    memset(P(f), 0, FILE_SIZE);
    WR32(f + F_FILE, h);
    WR32(f + F_FLAG, (plus ? IOREAD | IOWRITE : m[0] == 'r' ? IOREAD : IOWRITE) | IONBF);
    if (m[0] == 'a') {                                                  /* OS_Args: to the end */
        R[0] = 2; R[1] = h; do_swi(9, 0);
        R[0] = 1; R[1] = h; do_swi(9, 0);
    }
    fs[k].open = 1; fs[k].ungot = -1;
    return f;
}
static int c_fclose(uint32_t f)
{
    int k = file_index(f);
    if (k < 0 || !fs[k].open) return -1;
    if (k >= 3) { R[0] = 0; R[1] = RD32(f + F_FILE); do_swi(0x0D, 0); fs[k].open = 0; WR32(f + F_FLAG, 0); }
    return 0;
}
static uint32_t c_fread(uint32_t buf, uint32_t size, uint32_t n, uint32_t f)
{
    int k = file_index(f);
    if (k < 0 || !fs[k].open || !size) return 0;
    uint32_t total = size * n, got = 0;
    if (k == 0) {
        for (; got < total; got++) { int c = file_getc(f); if (c < 0) break; MEM[buf + got] = c; }
    } else {
        if (fs[k].ungot >= 0 && total) { MEM[buf] = fs[k].ungot; fs[k].ungot = -1; got = 1; }
        R[0] = 4; R[1] = RD32(f + F_FILE); R[2] = buf + got; R[3] = total - got;
        if (do_swi(0x0C, 0)) set_flag(f, IOERR, 0);                     /* OS_GBPB 4 */
        else got = total - R[3];
        if (got < total) set_flag(f, IOEOF, 0);
    }
    return got / size;
}
static uint32_t c_fwrite(uint32_t buf, uint32_t size, uint32_t n, uint32_t f)
{
    int k = file_index(f);
    if (k < 0 || !fs[k].open || !size) return 0;
    uint32_t total = size * n;
    if (k == 1 || k == 2) { file_puts(f, P(buf), total); return n; }
    R[0] = 2; R[1] = RD32(f + F_FILE); R[2] = buf; R[3] = total;
    if (do_swi(0x0C, 0)) { set_flag(f, IOERR, 0); return 0; }          /* OS_GBPB 2 */
    return (total - R[3]) / size;
}
static uint32_t c_fseek(uint32_t f, int32_t off, int whence)
{
    int k = file_index(f);
    if (k < 3 || !fs[k].open) return (uint32_t)-1;
    uint32_t h = RD32(f + F_FILE);
    if (whence == 1) { R[0] = 0; R[1] = h; do_swi(9, 0); off += R[2]; }     /* OS_Args 0: pointer */
    if (whence == 2) { R[0] = 2; R[1] = h; do_swi(9, 0); off += R[2]; }     /* OS_Args 2: extent */
    R[0] = 1; R[1] = h; R[2] = off;
    if (do_swi(9, 0)) return (uint32_t)-1;
    fs[k].ungot = -1;
    set_flag(f, 0, IOEOF);
    return 0;
}

/* -------------------------------------------------------------- printf */
/* Formatted output goes to an emulated buffer (sprintf) or a FILE. Each
 * conversion is done by the host's printf, with the argument fetched as
 * this ABI passes it (ints and pointers a word, doubles two). */
struct sink { uint32_t buf, f; int n; };
static void sink_put(struct sink *s, const char *p, int n)
{
    if (s->buf) memcpy(P(s->buf + s->n), p, n);
    else file_puts(s->f, p, n);
    s->n += n;
}

static int c_format(struct sink *s, uint32_t fmt, struct args *a)
{
    const char *p = P(fmt);
    while (*p) {
        if (*p != '%') {
            const char *q = p;
            while (*q && *q != '%') q++;
            sink_put(s, p, q - p);
            p = q;
            continue;
        }
        char spec[48], out[512];
        int sl = 0;
        spec[sl++] = *p++;
        while (*p && strchr("-+ #0", *p) && sl < 20) spec[sl++] = *p++;
        if (*p == '*') { sl += snprintf(spec + sl, 12, "%d", (int32_t)arg_word(a)); p++; }
        else while (*p >= '0' && *p <= '9' && sl < 30) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') { sl += snprintf(spec + sl, 12, "%d", (int32_t)arg_word(a)); p++; }
            else while (*p >= '0' && *p <= '9' && sl < 42) spec[sl++] = *p++;
        }
        char len = 0;
        if (*p == 'h' || *p == 'l' || *p == 'L') len = *p++;
        char conv = *p;
        if (!conv) break;
        p++;
        int n = 0;
        switch (conv) {
        case 'd': case 'i': {
            int32_t v = (int32_t)arg_word(a);
            if (len == 'h') v = (int16_t)v;
            spec[sl++] = 'd'; spec[sl] = 0;
            n = snprintf(out, sizeof out, spec, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'p': {
            uint32_t v = arg_word(a);
            if (len == 'h') v &= 0xFFFF;
            spec[sl++] = conv == 'p' ? 'x' : conv; spec[sl] = 0;
            n = snprintf(out, sizeof out, spec, v);
            break;
        }
        case 'c':
            spec[sl++] = 'c'; spec[sl] = 0;
            n = snprintf(out, sizeof out, spec, (int)(arg_word(a) & 0xFF));
            break;
        case 's': {
            uint32_t v = arg_word(a);
            spec[sl++] = 's'; spec[sl] = 0;
            const char *str = v ? P(v) : "<NULL>";
            int need = snprintf(NULL, 0, spec, str);
            char *o = need < (int)sizeof out ? out : malloc(need + 1);
            snprintf(o, need + 1, spec, str);
            sink_put(s, o, need);
            if (o != out) free(o);
            continue;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            double d = arg_double(a);
            spec[sl++] = conv; spec[sl] = 0;
            n = snprintf(out, sizeof out, spec, d);
            break;
        }
        case 'n': {
            uint32_t v = arg_word(a);
            if (len == 'h') { WR8(v, s->n); WR8(v + 1, s->n >> 8); } else WR32(v, s->n);
            continue;
        }
        case '%': out[0] = '%'; n = 1; break;
        default: spec[sl++] = conv; sink_put(s, spec, sl); continue;
        }
        if (n > (int)sizeof out - 1) n = sizeof out - 1;
        sink_put(s, out, n);
    }
    if (s->buf) MEM[s->buf + s->n] = 0;
    return s->n;
}

/* --------------------------------------------------------------- scanf */
/* Input from a string (sscanf) or a FILE, a character at a time. */
struct src { const char *str; uint32_t f; int pos, peeked; };
static int src_get(struct src *s)
{
    int c;
    if (s->peeked != -2) { c = s->peeked; s->peeked = -2; }
    else if (s->str) c = s->str[s->pos] ? (unsigned char)s->str[s->pos] : -1;
    else c = file_getc(s->f);
    if (c >= 0) s->pos++;
    return c;
}
static void src_unget(struct src *s, int c)
{
    if (c < 0) return;
    s->pos--;
    if (!s->str) s->peeked = c;
}
static int isws(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int c_scan(struct src *s, uint32_t fmt, struct args *a)
{
    const char *p = P(fmt);
    int assigned = 0, c = 0;
    while (*p) {
        if (isws((unsigned char)*p)) {
            while ((c = src_get(s)) >= 0 && isws(c)) ;
            src_unget(s, c);
            p++;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') p++;
            c = src_get(s);
            if (c != (unsigned char)*p) { src_unget(s, c); break; }
            p++;
            continue;
        }
        p++;
        int skip = 0, width = 0;
        char len = 0;
        if (*p == '*') { skip = 1; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + *p++ - '0';
        if (*p == 'h' || *p == 'l' || *p == 'L') len = *p++;
        char conv = *p++;
        if (!conv) break;
        if (!width) width = conv == 'c' ? 1 : 1 << 30;
        char buf[512];
        int n = 0;
        if (conv == 'n') { if (!skip) WR32(arg_word(a), s->pos); continue; }
        if (conv != 'c' && conv != '[') {
            while ((c = src_get(s)) >= 0 && isws(c)) ;
            src_unget(s, c);
        }
        if (conv == 'c') {
            while (n < width && n < (int)sizeof buf && (c = src_get(s)) >= 0) buf[n++] = c;
            if (n < width) break;
            if (!skip) { memcpy(P(arg_word(a)), buf, n); assigned++; }
            continue;
        }
        if (conv == 's' || conv == '[') {
            char set[256] = {0};
            int neg = 0;
            if (conv == '[') {
                if (*p == '^') { neg = 1; p++; }
                if (*p == ']') set[(unsigned char)*p++] = 1;
                while (*p && *p != ']') {
                    if (p[1] == '-' && p[2] && p[2] != ']') {
                        for (int x = (unsigned char)p[0]; x <= (unsigned char)p[2]; x++) set[x] = 1;
                        p += 3;
                    } else set[(unsigned char)*p++] = 1;
                }
                if (*p == ']') p++;
            }
            while (n < width && n < (int)sizeof buf - 1 && (c = src_get(s)) >= 0) {
                if (conv == 's' ? isws(c) : !(set[c] ^ neg)) { src_unget(s, c); break; }
                buf[n++] = c;
            }
            if (!n) break;
            if (!skip) { uint32_t d = arg_word(a); memcpy(P(d), buf, n); MEM[d + n] = 0; assigned++; }
            continue;
        }
        /* a number: the characters that may belong to one, then strtoX */
        int fp = strchr("eEfgG", conv) != 0;
        const char *ok = fp ? "0123456789+-.eE" : strchr("xXip", conv) ? "0123456789abcdefABCDEFxX+-" : "0123456789+-";
        while (n < width && n < (int)sizeof buf - 1 && (c = src_get(s)) >= 0 && strchr(ok, c)) buf[n++] = c;
        if (c >= 0 && n < width && n < (int)sizeof buf - 1) src_unget(s, c);
        buf[n] = 0;
        char *end;
        if (fp) {
            double d = strtod(buf, &end);
            if (end == buf) break;
            if (!skip) {
                uint32_t dst = arg_word(a);
                if (len == 'l' || len == 'L') put_double(dst, d);
                else { float fl = (float)d; uint32_t b; memcpy(&b, &fl, 4); WR32(dst, b); }
                assigned++;
            }
        } else {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : conv == 'i' ? 0 : 16;
            long v = conv == 'd' || conv == 'i' ? strtol(buf, &end, base) : (long)strtoul(buf, &end, base);
            if (end == buf) break;
            if (!skip) {
                uint32_t dst = arg_word(a);
                if (len == 'h') { WR8(dst, v); WR8(dst + 1, v >> 8); } else WR32(dst, (uint32_t)v);
                assigned++;
            }
        }
    }
    return assigned || c >= 0 ? assigned : -1;
}

/* ------------------------------------------------------- the rest's state */
static uint32_t rnd[55];
static int rnd_j, rnd_k, rnd_seeded;
static void c_srand(uint32_t seed)
{
    /* Knuth's additive generator (vol. 2, 3.2.2), its 55 words expanded
     * from the seed by a linear congruential generator, as the library does */
    rnd_j = 23; rnd_k = 54;
    for (int i = 0; i < 55; i++) { rnd[i] = seed + (seed >> 16); seed = 69069 * seed + 1725307361; }
    rnd_seeded = 1;
}
static uint32_t c_rand(void)
{
    /* (the library starts from a fixed table of its own; that isn't
     * reproduced here: before any srand() the sequence is srand(1)'s) */
    if (!rnd_seeded) c_srand(1);
    uint32_t t = rnd[rnd_k] += rnd[rnd_j];
    if (--rnd_j == 0) rnd_j = 54, --rnd_k;
    else if (--rnd_k == 0) rnd_k = 54;
    return t & 0x7FFFFFFF;
}

static uint32_t atexit_fn[34];
static int natexit;
static uint32_t signal_handler[NSIG];     /* 0: SIG_DFL */
static uint32_t strtok_next;
static uint32_t static_buf;               /* the library's own results: asctime, strerror, tmpnam... */

static uint32_t entry_addr(int k) { return k < (int)chunk[2].n ? chunk[2].vec + 4 * k : 0; }
#define SIG_DFL entry_addr(C___default_signal_handler)
#define SIG_IGN entry_addr(C___ignore_signal_handler)
#define SIG_ERR entry_addr(C___error_signal_marker)

static void c_exit(uint32_t status) __attribute__((noreturn));
static void c_exit(uint32_t status)
{
    while (natexit > 0) call_client(atexit_fn[--natexit], 0, 0, 0, 0);
    for (int k = 3; k < NFILES; k++) if (fs[k].open) c_fclose(file_addr(k));
    ev("clib_exit", "i", status);
    R[0] = 0; R[1] = 0x58454241; R[2] = status;                 /* "ABEX" and the return code */
    roger_swi(0x11);                                            /* OS_Exit */
    finish("exit");
}

static void c_raise(uint32_t sig)
{
    uint32_t h = sig < NSIG ? signal_handler[sig] : 0;
    if (h && h == SIG_IGN) return;
    if (!h || h == SIG_DFL || h == SIG_ERR) {
        fprintf(stderr, "SharedCLibrary: signal %u, default action: exit\n", sig);
        ev("clib_signal", "i", sig);
        c_exit(1);
    }
    signal_handler[sig] = 0;                                    /* reset to SIG_DFL, as ANSI C */
    call_client(h, sig, 0, 0, 0);
}

static time_t host_time(void)
{
    return roger_interactive() ? time(NULL) : (time_t)946684800;   /* headless: fixed (2000-01-01) */
}
static uint32_t put_tm(const struct tm *t)
{
    uint32_t a = static_buf + 128;
    int v[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday,
                t->tm_yday, t->tm_isdst};
    for (int k = 0; k < 9; k++) WR32(a + 4 * k, v[k]);
    return a;
}
static void get_tm(uint32_t a, struct tm *t)
{
    memset(t, 0, sizeof *t);
    t->tm_sec = RD32(a); t->tm_min = RD32(a + 4); t->tm_hour = RD32(a + 8); t->tm_mday = RD32(a + 12);
    t->tm_mon = RD32(a + 16); t->tm_year = RD32(a + 20); t->tm_wday = RD32(a + 24);
    t->tm_yday = RD32(a + 28); t->tm_isdst = RD32(a + 32);
}

/* Divisions (a2 / a1; results R0 and R1), a zero divisor raising SIGFPE */
static int div0(void) { if (A[0]) return 0; c_raise(SIGFPE); R[0] = R[1] = 0; return 1; }
static void udiv(void) { if (!div0()) { R[0] = A[1] / A[0]; R[1] = A[1] % A[0]; } }
static void sdiv(void)
{
    if (div0()) return;
    int32_t n = A[1], d = A[0];
    if (d == -1) { R[0] = -(uint32_t)n; R[1] = 0; }
    else { R[0] = n / d; R[1] = n % d; }
}

/* ------------------------------------------------- the kernel's functions */
/* Each returns 1 to return to the caller (R14, PSR restored, R4-R14 as at
 * the call), 0 when it has set npc itself. */
static int kernel_call(int k)
{
    uint32_t r, save[16];
    switch (k) {
    case K__kernel_init: {
        /* r0: kernel init block {image base, language descriptions start,
         * end}; r1/r2: root stack chunk base and top; r3: module?; r4:
         * workspace end. Set up the stack, call each language's Init (it
         * returns the main program's entry, if it has one), then run that. */
        uint32_t blkp = A[0], base = A[1], top = A[2];
        uint32_t ld = RD32(blkp + 4), le = RD32(blkp + 8), mainp = 0;
        WR32(base + SC_SLOFFSET + SL_LIB_OFFSET, 0);            /* static data relocations: none */
        WR32(base + SC_SLOFFSET + SL_LIB_OFFSET + 4, 0);
        R[13] = top; R[10] = base + SC_SLOFFSET; R[11] = 0;
        while (ld < le) {
            uint32_t size = RD32(ld);
            if (size < 4) break;
            if (size > 16 && RD32(ld + 16)) {                   /* lang_Init */
                uint32_t m = call_client(RD32(ld + 16), ld, 0, 0, 0);
                if (m) mainp = m;
            }
            ld += size;
        }
        if (!mainp) finish("C program: no main program");
        R[14] = entry_addr(C_exit);                             /* when it returns: exit(R0) */
        npc = mainp & 0x3FFFFFC;
        return 0;
    }
    case K__kernel_exit: c_exit(0);
    case K__kernel_setreturncode: return 1;
    case K__kernel_procname: case K__kernel_language: R[0] = 0; return 1;
    case K__kernel_command_string: do_swi(0x10, 0); return 1;  /* OS_GetEnv's */
    case K__kernel_hostos: R[0] = 6; return 1;                  /* RISC OS */
    case K__kernel_swi: case K__kernel_swi_c: {                 /* (no, in, out[, carry]) */
        for (int i = 0; i < 10; i++) R[i] = RD32(A[1] + 4 * i);
        uint32_t e = do_swi(A[0] & ~0x80000000u, (A[0] & 0x80000000u) != 0);
        if (!e) {
            for (int i = 0; i < 10; i++) WR32(A[2] + 4 * i, R[i]);
            if (k == K__kernel_swi_c) WR32(A[3], CF);
        }
        R[0] = e;
        return 1;
    }
    case K__kernel_osbyte:
        r = do_swi(6, 0) ? (uint32_t)-2 : (R[1] & 0xFF) | (R[2] & 0xFF) << 8 | (CF ? 0x10000 : 0);
        R[0] = r; return 1;
    case K__kernel_osrdch: r = do_swi(4, 0) ? (uint32_t)-2 : CF ? (uint32_t)-1 : R[0]; R[0] = r; return 1;
    case K__kernel_oswrch: do_swi(0, 0); return 1;
    case K__kernel_osbget: R[1] = A[0]; r = do_swi(0x0A, 0) ? (uint32_t)-2 : CF ? (uint32_t)-1 : R[0]; R[0] = r; return 1;
    case K__kernel_osbput: r = do_swi(0x0B, 0) ? (uint32_t)-2 : A[0]; R[0] = r; return 1;
    case K__kernel_osgbpb: {                                    /* (op, handle, block) */
        uint32_t b = A[2];
        for (int i = 0; i < 5; i++) R[2 + i] = RD32(b + 4 * i);
        uint32_t e = do_swi(0x0C, 0);
        if (!e) for (int i = 0; i < 3; i++) WR32(b + 4 * i, R[2 + i]);
        R[0] = e ? (uint32_t)-2 : CF ? (uint32_t)-1 : 0;
        return 1;
    }
    case K__kernel_osword: r = do_swi(7, 0) ? (uint32_t)-2 : R[1]; R[0] = r; return 1;
    case K__kernel_osfind: r = do_swi(0x0D, 0) ? (uint32_t)-2 : R[0]; R[0] = r; return 1;
    case K__kernel_osfile: {                                    /* (op, name, block) */
        uint32_t b = A[2];
        for (int i = 0; i < 4; i++) R[2 + i] = RD32(b + 4 * i);
        uint32_t e = do_swi(8, 0);
        if (!e) for (int i = 0; i < 4; i++) WR32(b + 4 * i, R[2 + i]);
        R[0] = e ? (uint32_t)-2 : R[0];
        return 1;
    }
    case K__kernel_osargs:
        r = do_swi(9, 0) ? (uint32_t)-2 : (A[0] == 0 && A[1] == 0) ? R[0] : R[2]; R[0] = r; return 1;
    case K__kernel_oscli: R[0] = do_swi(5, 0) ? (uint32_t)-2 : 1; return 1;
    case K__kernel_system: R[0] = do_swi(5, 0) ? (uint32_t)-2 : 0; return 1;
    case K__kernel_last_oserror:
        R[0] = chunk[1].data + K_ERROR;
        if (!RD32(R[0]) && !MEM[R[0] + 4]) R[0] = 0;
        return 1;
    case K__kernel_getenv: {                                    /* (name, buf, size) */
        R[1] = A[1]; R[2] = A[2] ? A[2] - 1 : 0; R[3] = 0; R[4] = 3;
        uint32_t e = do_swi(0x23, 0);                           /* OS_ReadVarVal, expanded */
        if (!e && A[2]) MEM[A[1] + R[2]] = 0;
        R[0] = e; return 1;
    }
    case K__kernel_setenv:                                      /* (name, value) */
        R[2] = A[1] ? strlen(P(A[1])) : (uint32_t)-1; R[3] = 0; R[4] = 4;
        R[0] = do_swi(0x24, 0); return 1;                       /* OS_SetVarVal */
    case K__kernel_stkovf_split_0frame: case K__kernel_stkovf_split:
        finish("C program: stack overflow (no stack extension)");
    case K__kernel_udiv: udiv(); return 1;
    case K__kernel_urem: if (!div0()) R[0] = A[1] % A[0]; return 1;
    case K__kernel_udiv10: R[0] = A[0] / 10; R[1] = A[0] % 10; return 1;
    case K__kernel_sdiv: sdiv(); return 1;
    case K__kernel_srem: if (!div0()) R[0] = (int32_t)A[0] == -1 ? 0 : (uint32_t)((int32_t)A[1] % (int32_t)A[0]); return 1;
    case K__kernel_sdiv10: R[0] = (int32_t)A[0] / 10; R[1] = (int32_t)A[0] % 10; return 1;
    case K__kernel_fpavailable: R[0] = 0; return 1;             /* no FP emulator */
    case K__kernel_irqs_on: do_swi(0x14, 0); return 1;          /* OS_IntOn */
    case K__kernel_irqs_off: do_swi(0x13, 0); return 1;         /* OS_IntOff */
    case K__kernel_irqs_disabled: R[0] = (MODEBITS & 0x08000000) != 0; return 1;
    case K__kernel_escape_seen: R[0] = 0; return 1;
    }
    (void)save;
    fail("not implemented:", kernel_names[k]);
    return 1;
}

/* ------------------------------------------------ the C library's functions */
static int clib_call(int k)
{
    struct args a = {0, 0};
    struct sink sk = {0, 0, 0};
    uint32_t r;
    switch (k) {
    case C_x_stack_overflow: case C_x_stack_overflow_1:
        finish("C program: stack overflow (no stack extension)");
    case C_x_udivide: udiv(); return 1;
    case C_x_divide: sdiv(); return 1;
    case C_x_uremainder: return kernel_call(K__kernel_urem);
    case C_x_remainder: return kernel_call(K__kernel_srem);
    case C_x_divtest: div0(); return 1;
    case C_x_multiply: R[0] = A[0] * A[1]; return 1;
    case C__main: {                                             /* (command string, main) */
        const char *c = P(A[0]);
        size_t l = strlen(c);
        uint32_t strs = c_malloc(l + 1), argv = c_malloc(4 * (l / 2 + 2)), o = strs;
        int argc = 0;
        while (*c) {
            while (*c == ' ') c++;
            if (!*c) break;
            WR32(argv + 4 * argc++, o);
            int q = 0;
            while (*c && (q || *c != ' ')) {
                if (*c == '"') { q = !q; c++; continue; }
                MEM[o++] = *c++;
            }
            MEM[o++] = 0;
        }
        WR32(argv + 4 * argc, 0);
        R[0] = argc; R[1] = argv;
        R[14] = entry_addr(C_exit);                             /* main returning: exit(R0) */
        npc = A[1] & 0x3FFFFFC;
        return 0;
    }
    case C__exit: case C_exit: c_exit(A[0]);
    case C__clib_initialise: return 1;                          /* (done at LibInit) */
    case C__printf: case C_printf: a.k = 1; sk.f = file_addr(1); R[0] = c_format(&sk, A[0], &a); return 1;
    case C__fprintf: case C_fprintf: a.k = 2; sk.f = A[0]; R[0] = c_format(&sk, A[1], &a); return 1;
    case C__sprintf: case C_sprintf: a.k = 2; sk.buf = A[0]; R[0] = c_format(&sk, A[1], &a); return 1;
    case C_vprintf: a.mem = A[1]; sk.f = file_addr(1); R[0] = c_format(&sk, A[0], &a); return 1;
    case C_vfprintf: case C__vfprintf: a.mem = A[2]; sk.f = A[0]; R[0] = c_format(&sk, A[1], &a); return 1;
    case C_vsprintf: a.mem = A[2]; sk.buf = A[0]; R[0] = c_format(&sk, A[1], &a); return 1;
    case C_scanf: { struct src s = {NULL, file_addr(0), 0, -2}; a.k = 1; R[0] = c_scan(&s, A[0], &a); return 1; }
    case C_fscanf: { struct src s = {NULL, A[0], 0, -2}; a.k = 2; R[0] = c_scan(&s, A[1], &a); return 1; }
    case C_sscanf: { struct src s = {P(A[0]), 0, 0, -2}; a.k = 2; R[0] = c_scan(&s, A[1], &a); return 1; }
    case C_clock: R[0] = (uint32_t)(icount / TICK_STEPS); return 1;          /* centiseconds */
    case C_difftime: ret_double((double)(int32_t)A[0] - (double)(int32_t)A[1]); return 1;
    case C_mktime: { struct tm t; get_tm(A[0], &t); R[0] = (uint32_t)mktime(&t); return 1; }
    case C_time: { time_t t = host_time(); if (A[0]) WR32(A[0], (uint32_t)t); R[0] = (uint32_t)t; return 1; }
    case C_asctime: case C_ctime: {
        struct tm t;
        if (k == C_asctime) get_tm(A[0], &t);
        else { time_t tt = (int32_t)RD32(A[0]); localtime_r(&tt, &t); }
        char b[64];
        asctime_r(&t, b);
        strcpy(P(static_buf), b);
        R[0] = static_buf;
        return 1;
    }
    case C_gmtime: case C_localtime: {
        time_t tt = (int32_t)RD32(A[0]);
        struct tm t;
        if (k == C_gmtime) gmtime_r(&tt, &t); else localtime_r(&tt, &t);
        R[0] = put_tm(&t);
        return 1;
    }
    case C_strftime: { struct tm t; get_tm(A[3], &t); R[0] = strftime(P(A[0]), A[1], P(A[2]), &t); return 1; }
    case C_memcpy: case C_memmove: case C__memcpy: memmove(P(A[0]), P(A[1]), A[2]); return 1;
    case C_memset: case C__memset: memset(P(A[0]), A[1] & 0xFF, A[2]); return 1;
    /* (forwards a byte at a time, as a program whose operands overlap
     * would have seen it - not the host's, undefined then) */
    case C_strcpy: copy_str(A[0], A[1], 0xFFFFFFFFu, 0); return 1;
    case C_strncpy: copy_str(A[0], A[1], A[2], 1); return 1;
    case C_strcat: copy_str(A[0] + strlen(P(A[0])), A[1], 0xFFFFFFFFu, 0); return 1;
    case C_strncat: { uint32_t d = A[0] + strlen(P(A[0])); d = copy_str(d, A[1], A[2], 0); MEM[d] = 0; return 1; }
    case C_memcmp: R[0] = sign(memcmp(P(A[0]), P(A[1]), A[2])); return 1;
    case C_strcmp: case C_strcoll: R[0] = sign(strcmp(P(A[0]), P(A[1]))); return 1;
    case C_strncmp: R[0] = sign(strncmp(P(A[0]), P(A[1]), A[2])); return 1;
    case C_memchr: R[0] = at(A[0], memchr(P(A[0]), A[1] & 0xFF, A[2])); return 1;
    case C_strchr: R[0] = at(A[0], strchr(P(A[0]), A[1] & 0xFF)); return 1;
    case C_strrchr: R[0] = at(A[0], strrchr(P(A[0]), A[1] & 0xFF)); return 1;
    case C_strpbrk: R[0] = at(A[0], strpbrk(P(A[0]), P(A[1]))); return 1;
    case C_strstr: R[0] = at(A[0], strstr(P(A[0]), P(A[1]))); return 1;
    case C_strcspn: R[0] = strcspn(P(A[0]), P(A[1])); return 1;
    case C_strspn: R[0] = strspn(P(A[0]), P(A[1])); return 1;
    case C_strlen: R[0] = strlen(P(A[0])); return 1;
    case C_strxfrm: { size_t l = strlen(P(A[1])); if (l < A[2]) copy_str(A[0], A[1], 0xFFFFFFFFu, 0); R[0] = l; return 1; }
    case C_strtok: {
        uint32_t p = A[0] ? A[0] : strtok_next;
        R[0] = 0;
        if (!p) return 1;
        p += strspn(P(p), P(A[1]));
        if (!MEM[p]) { strtok_next = 0; return 1; }
        uint32_t e = p + strcspn(P(p), P(A[1]));
        if (MEM[e]) { MEM[e] = 0; strtok_next = e + 1; } else strtok_next = 0;
        R[0] = p;
        return 1;
    }
    case C_strerror: snprintf(P(static_buf), 120, "%s", strerror((int)A[0])); R[0] = static_buf; return 1;
    case C_atof: ret_double(atof(P(A[0]))); return 1;
    case C_atoi: case C_atol: R[0] = (uint32_t)(int32_t)atol(P(A[0])); return 1;
    case C_strtod: { char *e; double d = strtod(P(A[0]), &e); if (A[1]) WR32(A[1], at(A[0], e)); ret_double(d); return 1; }
    case C_strtol: case C_strtoul: {
        char *e;
        r = k == C_strtol ? (uint32_t)(int32_t)strtol(P(A[0]), &e, A[2]) : (uint32_t)strtoul(P(A[0]), &e, A[2]);
        if (A[1]) WR32(A[1], at(A[0], e));
        R[0] = r;
        return 1;
    }
    case C_rand: R[0] = c_rand(); return 1;
    case C_srand: c_srand(A[0]); return 1;
    case C_calloc: r = A[0] * A[1]; R[0] = c_malloc(r); if (R[0]) memset(P(R[0]), 0, r); return 1;
    case C_free: c_free(A[0]); return 1;
    case C_malloc: R[0] = c_malloc(A[0]); return 1;
    case C_realloc: R[0] = c_realloc(A[0], A[1]); return 1;
    case C_abort: c_raise(SIGABRT); c_exit(1);
    case C_atexit: if (natexit < 34) { atexit_fn[natexit++] = A[0]; R[0] = 0; } else R[0] = 1; return 1;
    case C_getenv: {
        uint32_t buf = static_buf + 256;
        R[1] = buf; R[2] = 255; R[3] = 0; R[4] = 3;
        if (do_swi(0x23, 0)) R[0] = 0;                          /* OS_ReadVarVal */
        else { MEM[buf + R[2]] = 0; R[0] = buf; }
        return 1;
    }
    case C_system:                                              /* a * command */
        if (!A[0]) { R[0] = 1; return 1; }
        R[0] = do_swi(5, 0) ? (uint32_t)-2 : 0;
        return 1;
    case C_abs: case C_labs: R[0] = (int32_t)A[0] < 0 ? -A[0] : A[0]; return 1;
    case C_div: case C_ldiv: {                                  /* (result, n, d): the struct at a1 */
        int32_t n = A[1], d = A[2];
        if (!d) { c_raise(SIGFPE); return 1; }
        WR32(A[0], d == -1 ? -(uint32_t)n : (uint32_t)(n / d));
        WR32(A[0] + 4, d == -1 ? 0 : (uint32_t)(n % d));
        return 1;
    }
    case C_remove: R[0] = 6; R[1] = A[0]; R[0] = do_swi(8, 0) ? 1 : 0; return 1;   /* OS_File 6 */
    case C_rename: R[0] = 25; R[1] = A[0]; R[2] = A[1]; R[0] = do_swi(0x29, 0) ? 1 : 0; return 1;  /* OS_FSControl 25 */
    case C_fopen: R[0] = c_fopen(A[0], A[1]); return 1;
    case C_fclose: R[0] = c_fclose(A[0]); return 1;
    case C_fflush: R[0] = 0; return 1;                          /* unbuffered */
    case C_setbuf: case C_setvbuf: R[0] = 0; return 1;          /* always unbuffered */
    case C_fgetc: case C___filbuf: case C_getc: R[0] = file_getc(A[0]); return 1;
    case C_getchar: R[0] = file_getc(file_addr(0)); return 1;
    case C_fgets: case C_gets: {                                /* fgets(s, n, f), gets(s) */
        uint32_t buf = A[0], n = k == C_fgets ? A[1] : 0x7FFFFFFF, f = k == C_fgets ? A[2] : file_addr(0), i = 0;
        int c = -1;
        while (i + 1 < n && (c = file_getc(f)) >= 0) {
            if (c == '\n' && k == C_gets) break;
            MEM[buf + i++] = c;
            if (c == '\n') break;
        }
        MEM[buf + i] = 0;
        R[0] = i == 0 && c < 0 ? 0 : buf;
        return 1;
    }
    case C_fputc: case C___flsbuf: case C_putc: R[0] = file_putc(A[0], A[1]); return 1;
    case C_putchar: R[0] = file_putc(A[0], file_addr(1)); return 1;
    case C_fputs: case C_puts: {
        uint32_t f = k == C_fputs ? A[1] : file_addr(1);
        file_puts(f, P(A[0]), strlen(P(A[0])));
        if (k == C_puts) file_putc('\n', f);
        R[0] = 0;
        return 1;
    }
    case C_ungetc: {
        int i = file_index(A[1]);
        if (i < 0 || A[0] == (uint32_t)-1) { R[0] = (uint32_t)-1; return 1; }
        fs[i].ungot = A[0] & 0xFF;
        set_flag(A[1], 0, IOEOF);
        R[0] = A[0] & 0xFF;
        return 1;
    }
    case C_fread: R[0] = c_fread(A[0], A[1], A[2], A[3]); return 1;
    case C_fwrite: R[0] = c_fwrite(A[0], A[1], A[2], A[3]); return 1;
    case C_fseek: R[0] = c_fseek(A[0], (int32_t)A[1], A[2]); return 1;
    case C_rewind: c_fseek(A[0], 0, 0); set_flag(A[0], 0, IOERR); return 1;
    case C_ftell: {
        int i = file_index(A[0]);
        R[0] = 0; R[1] = i >= 3 ? RD32(A[0] + F_FILE) : 0;
        R[0] = i < 3 || do_swi(9, 0) ? (uint32_t)-1 : R[2] - (fs[i].ungot >= 0);
        return 1;
    }
    case C_clearerr: set_flag(A[0], 0, IOEOF | IOERR); return 1;
    case C_feof: R[0] = (RD32(A[0] + F_FLAG) & IOEOF) != 0; return 1;
    case C_ferror: R[0] = (RD32(A[0] + F_FLAG) & IOERR) != 0; return 1;
    case C_perror: {
        char m[300];
        const char *pre = A[0] && MEM[A[0]] ? P(A[0]) : "";
        snprintf(m, sizeof m, "%s%s%s\n", pre, *pre ? ": " : "", strerror(RD32(chunk[2].data + D_ERRNO)));
        file_puts(file_addr(2), m, strlen(m));
        return 1;
    }
    case C___ignore_signal_handler: return 1;
    case C___default_signal_handler: case C___error_signal_marker: c_raise(A[0]); return 1;
    case C_signal: {                                            /* (sig, handler) */
        if (A[0] >= NSIG || !A[0]) { R[0] = SIG_ERR; return 1; }
        uint32_t old = signal_handler[A[0]] ? signal_handler[A[0]] : SIG_DFL;
        signal_handler[A[0]] = A[1] == SIG_DFL ? 0 : A[1];
        R[0] = old;
        return 1;
    }
    case C_raise: c_raise(A[0]); R[0] = 0; return 1;
    case C_setlocale: R[0] = 0; return 1;                       /* only "C" */
    case C_isalnum: case C_isalpha: case C_iscntrl: case C_isdigit: case C_isgraph: case C_islower:
    case C_isprint: case C_ispunct: case C_isspace: case C_isupper: case C_isxdigit: {
        static int (*const f[])(int) = {isalnum, isalpha, iscntrl, isdigit, isgraph, islower, isprint,
                                        ispunct, isspace, isupper, isxdigit};
        int c = (int32_t)A[0];
        R[0] = c >= 0 && c < 128 && f[k - C_isalnum](c) != 0;
        return 1;
    }
    case C_tolower: case C_toupper: {
        int c = (int32_t)A[0];
        if (c >= 0 && c < 128) R[0] = k == C_tolower ? tolower(c) : toupper(c);
        return 1;
    }
    case C_tmpnam: case C___old_tmpnam: {
        static int n;
        uint32_t b = A[0] ? A[0] : static_buf + 512;
        snprintf(P(b), 32, "<Wimp$ScrapDir>.x%08d", n++);
        R[0] = b;
        return 1;
    }
    }
    fail("not implemented:", clib_names[k]);
    return 1;
}

/* ------------------------------------------------------------------ LibInit */
static void init_static(void)
{
    if (chunk[1].data) memset(P(chunk[1].data), 0, chunk[1].dlen);
    uint32_t d = chunk[2].data;
    if (!d) return;
    memset(P(d), 0, chunk[2].dlen);
    for (int c = 0; c < 128; c++) {                             /* the "C" locale's classes */
        uint8_t v = 0;
        if (isspace(c)) v |= 1;                                 /* __S */
        if (ispunct(c)) v |= 2;                                 /* __P */
        if (c == ' ') v |= 4;                                   /* __B */
        if (islower(c)) v |= 8;                                 /* __L */
        if (isupper(c)) v |= 16;                                /* __U */
        if (isdigit(c)) v |= 32;                                /* __N */
        if (iscntrl(c)) v |= 64;                                /* __C */
        if (isxdigit(c) && !isdigit(c)) v |= 128;               /* __X */
        MEM[d + D_CTYPE + c] = v;
    }
    if (chunk[2].dlen >= D_HUGEVAL + 8) { WR32(d + D_HUGEVAL, 0x7FEFFFFF); WR32(d + D_HUGEVAL + 4, 0xFFFFFFFF); }
    memset(fs, 0, sizeof fs);
    for (int k = 0; k < 3; k++) {                               /* stdin, stdout, stderr */
        WR32(file_addr(k) + F_FLAG, (k == 0 ? IOREAD : IOWRITE) | IONBF);
        fs[k].open = 1; fs[k].ungot = -1;
    }
}

int clib_swi(uint32_t b)
{
    if (b < 0x80680 || b > 0x80684) return 0;
    if (b != 0x80680 && b != 0x80681) {
        ev("clib_libinit_unsupported", "i", b);
        fprintf(stderr, "SharedCLibrary: SWI &%x (modules, 32-bit clients) not supported\n", b);
        finish("SharedCLibrary SWI not supported");
    }
    /* LibInitAPCS_A/R: r0 the chunk list, r1/r2 the workspace (from the
     * end of the image to the RAM limit), r6 the root stack size in K (top
     * half). Out: r0 the workspace limit, r1/r2 the root stack chunk's base
     * and top, r6 the library version. */
    memset(chunk, 0, sizeof chunk);
    for (uint32_t d = R[0]; RD32(d) != 0xFFFFFFFFu; d += 20) {
        uint32_t id = RD32(d);
        if (id < 1 || id > 2) { ev("clib_unknown_chunk", "i", id); continue; }
        chunk[id].vec = RD32(d + 4);
        chunk[id].n = (RD32(d + 8) - chunk[id].vec) / 4;
        chunk[id].data = RD32(d + 12);
        chunk[id].dlen = RD32(d + 16) - chunk[id].data;
        uint32_t limit = id == 1 ? NKERNEL : NCLIB;
        if (chunk[id].n > limit) { ev("clib_vector_too_long", "ii", id, chunk[id].n); chunk[id].n = limit; }
        for (uint32_t k = 0; k < chunk[id].n; k++) {
            uint32_t a = chunk[id].vec + 4 * k;
            if (cpu_translated(a)) smc_write(a);                /* translated code leaves at it */
            WR32(a, 0xE1B0F00Eu);                               /* (MOVS PC, R14: never run) */
        }
    }
    uint32_t ws = (R[1] + 7) & ~7u, limit = R[2], want = (R[6] >> 16) << 10;
    stack_base = ws;
    stack_top = ws + (want > ROOT_STACK ? want : ROOT_STACK);
    heap_lo = stack_top; heap_hi = limit;
    if (heap_hi < heap_lo + 0x2000) finish("SharedCLibrary: no room for the stack and heap (WimpSlot)");
    heap_init();
    init_static();
    natexit = 0; rnd_seeded = 0; strtok_next = 0;
    memset(signal_handler, 0, sizeof signal_handler);
    static_buf = c_malloc(1024);
    inited = 1;
    ev("clib_libinit", "iiii", stack_base, stack_top, heap_hi, chunk[2].n);
    R[0] = limit; R[1] = stack_base; R[2] = stack_top; R[6] = LIB_VERSION;
    return 1;
}

/* call_addr: an entry in a client's vector? Arguments are taken from R0-R3
 * and the stack; R4-R14 are as at the call when it returns (APCS). */
int clib_dispatch(uint32_t a)
{
    if (!inited) return 0;
    for (int id = 1; id <= 2; id++)
        if (a >= chunk[id].vec && a < chunk[id].vec + 4 * chunk[id].n) {
            int k = (a - chunk[id].vec) / 4;
            uint32_t save[16];
            memcpy(save, R, sizeof save);
            memcpy(A, R, sizeof A);
            SP = R[13];
            if (id == 1 ? kernel_call(k) : clib_call(k)) {
                memcpy(R + 4, save + 4, 11 * sizeof *R);
                SETPC(R[14], 1);
            }
            return 1;
        }
    return 0;
}
