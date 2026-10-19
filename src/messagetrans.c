/* MessageTrans: messages looked up by token in a messages file.
 *
 * A messages file is lines of text: "token:message", where several tokens
 * separated by "/" (or each on a line of its own, ending without a colon)
 * share a message, a "?" in a token matches any character, and "#" starts
 * a comment line. A message runs to the end of its line (a control
 * character).
 *
 * The SWIs (&41500 on), as the PRMs give them:
 *   0 FileInfo (R1 name -> R0 flags: bit 0 the file is in memory and used
 *     where it is, R2 the buffer size it needs)
 *   1 OpenFile (R0 a 16-byte file descriptor, R1 the name, R2 a buffer or
 *     0: a ResourceFS file is used where it is, another file loaded into
 *     the RMA)
 *   2 Lookup (R0 descriptor - 0 the global messages, which aren't here - R1
 *     the token, ending in a control character, or ":" and a default; R2
 *     a buffer and R3 its size, or R2 0: R2 -> the message in the file;
 *     R4-R7 parameters %0-%3 -> R2 the result, R3 its length)
 *   3 MakeMenus (not here)
 *   4 CloseFile (R0 descriptor)
 *   5 EnumerateTokens (R0 descriptor, R1 a token with wildcards, R2/R3 a
 *     buffer, R4 0 or the value from before -> R2 the token or 0, R4)
 *   6 ErrorLookup (R0 an error block: number and token; R1 descriptor; R2/R3
 *     a buffer or 0; R4-R7 -> R0 the error, V set; without X, raised)
 *   7 GSLookup (Lookup, then GSTrans)
 *   8 CopyError (R0 an error -> R0 a copy in MessageTrans's buffer)
 * The descriptor: +0 the file's address, +4 its length, +8 1 if MessageTrans
 * claimed the memory, +12 the name's address. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define ERRBUF_N 4
static uint32_t errbuf[ERRBUF_N];         /* error blocks returned (in the RMA), used in turn */
static int errnext;

static uint32_t error_buffer(void)
{
    if (!errbuf[errnext]) { R[0] = 6; R[3] = 256; roger_swi(0x2001E); errbuf[errnext] = R[2]; }
    uint32_t b = errbuf[errnext];
    errnext = (errnext + 1) % ERRBUF_N;
    return b;
}

/* a token in the file, at t (ending at ':' '/' or a control character),
 * matches the one asked for (ending at a control character or ':') */
static int token_match(uint32_t t, const char *want)
{
    for (;; t++, want++) {
        uint8_t c = MEM[t];
        int end = c == ':' || c == '/' || c < 32, wend = !*want;
        if (end || wend) return end && wend;
        if (c != '?' && c != (uint8_t)*want) return 0;
    }
}

/* the message for token in [f, f + n): its address and length, or 0 */
static uint32_t find(uint32_t f, uint32_t n, const char *token, uint32_t *len)
{
    uint32_t end = f + n, p = f;
    int matched = 0;
    while (p < end) {
        if (MEM[p] == '#') { while (p < end && MEM[p] >= 32) p++; p++; matched = 0; continue; }
        for (;;) {                                    /* a line's tokens */
            uint32_t t = p;
            while (p < end && MEM[p] >= 32 && MEM[p] != ':' && MEM[p] != '/') p++;
            if (p > t && token_match(t, token)) matched = 1;
            if (p >= end) return 0;
            if (MEM[p] == '/') { p++; continue; }
            if (MEM[p] == ':') {                      /* the message */
                uint32_t m = ++p;
                while (p < end && MEM[p] >= 32) p++;
                if (matched) { *len = p - m; return m; }
                matched = 0;
            } else if (p == t) matched = 0;           /* (an empty line) */
            p++;                                      /* (tokens only: more on the next line) */
            break;
        }
    }
    return 0;
}

/* the message (with %0-%3 from R4-R7) into out; its length */
static int substitute(uint32_t m, uint32_t len, char *out, int size, const uint32_t par[4])
{
    int o = 0;
    for (uint32_t k = 0; k < len && o < size - 1; k++) {
        char c = MEM[m + k];
        if (c == '%' && k + 1 < len && MEM[m + k + 1] >= '0' && MEM[m + k + 1] <= '3') {
            uint32_t a = par[MEM[m + k + 1] - '0'];
            k++;
            if (a) for (; MEM[a] >= 32 && o < size - 1; a++) out[o++] = MEM[a];
            continue;
        }
        if (c == '%' && k + 1 < len && MEM[m + k + 1] == '%') k++;
        out[o++] = c;
    }
    out[o] = 0;
    return o;
}

static int lookup(uint32_t desc, uint32_t tokp, char *out, int size, uint32_t *at, uint32_t *alen)
{
    char token[256];
    int n = 0;
    while (MEM[tokp + n] >= 32 && MEM[tokp + n] != ':' && n < 255) token[n] = MEM[tokp + n], n++;
    token[n] = 0;
    uint32_t len = 0, m = desc ? find(RD32(desc), RD32(desc + 4), token, &len) : 0;
    if (!m && MEM[tokp + n] == ':') {                 /* the default */
        m = tokp + n + 1;
        while (MEM[m + len] >= 32) len++;
    }
    if (!m) {
        ev("MessageTrans_token_not_found", "s", token, n);
        snprintf(out, size, "Message token %s not found", token);
        return 0;
    }
    uint32_t par[4] = {R[4], R[5], R[6], R[7]};
    substitute(m, len, out, size, par);
    if (at) { *at = m; *alen = len; }
    return 1;
}

static int open_file(uint32_t desc, uint32_t name, uint32_t buf)
{
    char n[1100], path[2048];
    cstr(name, n, 1);
    uint32_t data, len;
    if (!buf && resourcefs_file(n, &data, &len)) {
        WR32(desc, data); WR32(desc + 4, len); WR32(desc + 8, 0); WR32(desc + 12, name);
        return 1;
    }
    if (!hostpath_read(n, path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    int claimed = !buf;
    if (claimed) { R[0] = 6; R[3] = l + 1; roger_swi(0x2001E); buf = R[2]; }
    size_t got = fread(MEM + buf, 1, l, f);
    fclose(f);
    MEM[buf + got] = 10;
    WR32(desc, buf); WR32(desc + 4, got); WR32(desc + 8, claimed); WR32(desc + 12, name);
    return 1;
}

static void error_out(uint32_t num, const char *text)
{
    uint32_t e = error_buffer();
    WR32(e, num);
    snprintf((char *)MEM + e + 4, 252, "%s", text);
    R[0] = e;
    VF = 1;
}

int messagetrans_swi(uint32_t b, int x)
{
    if (b < 0x41500 || b > 0x4153F) return 0;
    char out[1024], name[1100];
    uint32_t at, len;
    switch (b - 0x41500) {
    case 0: {                                          /* FileInfo */
        char path[2048];
        cstr(R[1], name, 1);
        if (resourcefs_file(name, &at, &len)) { R[0] = 1; R[2] = len; return 1; }
        if (!hostpath_read(name, path)) { os_error(0xD6, "File not found"); return 1; }
        FILE *f = fopen(path, "rb");
        if (!f) { os_error(0xD6, "File not found"); return 1; }
        fseek(f, 0, SEEK_END);
        R[0] = 0; R[2] = ftell(f) + 1;
        fclose(f);
        return 1;
    }
    case 1:                                            /* OpenFile */
        if (!open_file(R[0], R[1], R[2])) {
            cstr(R[1], name, 1);
            ev("MessageTrans_OpenFile_failed", "s", name, (int)strlen(name));
            os_error(0xD6, "File not found");
        }
        return 1;
    case 2: case 7: {                                  /* Lookup, GSLookup */
        uint32_t buf = R[2], size = R[3];
        if (!lookup(R[0], R[1], out, sizeof out, &at, &len)) { error_out(0xAC2, out); return 1; }
        if (b - 0x41500 == 7) { char g[1024]; gstrans(out, g, sizeof g); strcpy(out, g); }
        if (!buf) { R[2] = at; R[3] = len; return 1; }  /* where it is in the file */
        int n = strlen(out);
        if (size && (uint32_t)n >= size) n = size - 1;
        memcpy(MEM + buf, out, n);
        MEM[buf + n] = 0;
        R[2] = buf; R[3] = n;
        return 1;
    }
    case 3: os_error(0x1E6, "MessageTrans_MakeMenus not supported"); return 1;
    case 4:                                            /* CloseFile */
        if (RD32(R[0] + 8)) { uint32_t d = R[0]; R[0] = 7; R[2] = RD32(d); roger_swi(0x2001E); R[0] = d; WR32(d + 8, 0); }
        return 1;
    case 5: {                                          /* EnumerateTokens */
        char want[256];
        cstr(R[1], want, 1);
        uint32_t f = RD32(R[0]), end = f + RD32(R[0] + 4), p = f, idx = 0;
        while (p < end) {                              /* each token in turn: the R4'th match on */
            if (MEM[p] == '#') { while (p < end && MEM[p] != 10) p++; p++; continue; }
            uint32_t t = p;
            while (p < end && MEM[p] >= 32 && MEM[p] != ':' && MEM[p] != '/') p++;
            int tl = p - t, ok = tl > 0 && tl < 255;
            for (int k = 0, w = 0; ok && (k < tl || want[w]); k++, w++) {
                if (want[w] == '*') { ok = 1; break; }
                if (k >= tl || !want[w] || (want[w] != '?' && want[w] != MEM[t + k])) ok = 0;
            }
            if (ok && idx++ == R[4]) {
                uint32_t n = tl < (int)R[3] ? (uint32_t)tl : R[3] - 1;
                memcpy(MEM + R[2], MEM + t, n);
                MEM[R[2] + n] = 0;
                R[3] = n; R[4] = idx;
                return 1;
            }
            if (p < end && MEM[p] == ':') while (p < end && MEM[p] != 10) p++;
            p++;
        }
        R[2] = 0;
        return 1;
    }
    case 6: {                                          /* ErrorLookup */
        uint32_t eb = R[0], buf = R[2], size = R[3];
        lookup(R[1], eb + 4, out, sizeof out, NULL, NULL);
        if (!buf) buf = error_buffer(), size = 256;
        WR32(buf, RD32(eb));
        int n = strlen(out);
        if ((uint32_t)n > size - 5) n = size - 5;
        memcpy(MEM + buf + 4, out, n);
        MEM[buf + 4 + n] = 0;
        R[0] = buf;
        VF = 1;
        if (!x) roger_swi(0x2B);                       /* OS_GenerateError */
        return 1;
    }
    case 8: {                                          /* CopyError */
        uint32_t e = error_buffer();
        memmove(MEM + e, MEM + R[0], 256);
        MEM[e + 255] = 0;
        R[0] = e;
        return 1;
    }
    }
    os_error(0x1E6, "SWI value out of range for module MessageTrans");
    return 1;
}
