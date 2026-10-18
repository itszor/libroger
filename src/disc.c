/* Disc-level access: the floppy disc in drive 0, read sector by sector as
 * ADFS's FileCore instance reads it (FileCore_DiscOp, ADFS_DiscOp) - what
 * copy protection checks. The files themselves are read from the host
 * directory (files.c); this is only for programs that go below them.
 *
 * The disc (roger_config.floppy, --floppy=) is an HxC .hfe image - its MFM
 * tracks are decoded here into the sectors on them, keeping each one's ID
 * (C, H, R, N) and whether its data CRC is good, so a protection track of
 * odd sizes and deliberate CRC errors reads as it did on the drive - or a
 * flat image (.adf: ADFS D/E, 800K, or F, 1600K; every sector good; a dump
 * short by whole tracks has those unformatted).
 *
 * A DiscOp addresses the disc as bytes, which a disc record turns into
 * (cylinder, head, sector): the disc's own geometry, or an alternative
 * record the caller passes (R1 bits 8-31, the record's address / 4). A
 * sector is found on its track by its ID's R, as the 1772 controller finds
 * it; a sector that isn't there, or whose data has a bad CRC, is a disc
 * error (the latter's data read into the buffer first, as the controller
 * transfers it before it finds the CRC wrong). Retries (ADFS_Retries) make no difference to a disc image.
 *
 * RISC OS's own modules aren't there to be looked at, but a program can
 * look one up (OS_Module 18) and call its SWI handler directly, past the
 * SWI vector. FileCore and ADFS are given module headers in "ROM"
 * (&3800000, where RISC OS 3.1's ROM is), whose SWI handlers are a few
 * ARM instructions that issue the SWI the caller asked for (interpreted:
 * see cpu.c), so such a call ends up here too; OS_FSControl 20 gives
 * ADFS's module base and FileCore instance word as RISC OS would. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define CYLS 84

struct sector { uint8_t c, h, r, n, ok; uint8_t *data; };
struct track { struct sector *s; int n; };
static struct track tracks[CYLS][2];
static int loaded;
static struct { int log2size, spt, heads, low; } geom;     /* the disc's own */
static uint32_t retries;                                   /* ADFS_Retries' word */

/* ---------------------------------------------------------------- images */
static void add_sector(int cyl, int head, struct sector s)
{
    struct track *t = &tracks[cyl][head];
    for (int k = 0; k < t->n; k++)                 /* seen again past the index: keep a good copy */
        if (t->s[k].r == s.r && t->s[k].n == s.n) {
            if (!t->s[k].ok && s.ok) { free(t->s[k].data); t->s[k] = s; } else free(s.data);
            return;
        }
    t->s = realloc(t->s, (t->n + 1) * sizeof *t->s);
    t->s[t->n++] = s;
}

static uint16_t crc16(const uint8_t *p, int n, uint16_t crc)
{
    while (n--) {
        crc ^= *p++ << 8;
        for (int k = 0; k < 8; k++) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* n bytes of MFM data from the cell stream at bit pos (the second cell of
 * each pair is the data bit); 0 if the stream ends first */
static int mfm_bytes(const uint8_t *cells, long ncells, long pos, uint8_t *out, int n)
{
    if (pos + 16L * n > ncells) return 0;
    for (int k = 0; k < n; k++) {
        int b = 0;
        for (int j = 0; j < 8; j++) {
            long c = pos + 16L * k + 2 * j + 1;
            b = b << 1 | ((cells[c >> 3] >> (c & 7)) & 1);
        }
        out[k] = b;
    }
    return 1;
}

/* the sectors in one side's track: IDAMs (A1 A1 A1 FE C H R N CRC) and
 * the data records after them (A1 A1 A1 FB/F8 data CRC) */
static void mfm_track(int cyl, int head, const uint8_t *raw, long len)
{
    long ncells = len * 16;                        /* the track twice: records round the index */
    uint8_t *cells = malloc(len * 2);
    memcpy(cells, raw, len); memcpy(cells + len, raw, len);
    const uint64_t sync = 0x448944894489ull;
    uint64_t sr = 0;
    int have_id = 0; long id_end = 0; uint8_t id[7];
    for (long i = 0; i < ncells; i++) {
        sr = (sr << 1 | ((cells[i >> 3] >> (i & 7)) & 1)) & 0xFFFFFFFFFFFFull;
        if (sr != sync) continue;
        long p = i + 1;
        uint8_t mark;
        if (!mfm_bytes(cells, ncells, p, &mark, 1)) break;
        static const uint8_t a1[3] = {0xA1, 0xA1, 0xA1};
        if (mark == 0xFE) {
            if (p >= len * 8 && !have_id) break;   /* past one revolution */
            if (mfm_bytes(cells, ncells, p, id, 7) &&
                crc16(id, 5, crc16(a1, 3, 0xFFFF)) == (id[5] << 8 | id[6])) {
                have_id = 1; id_end = p + 16 * 7;
            }
            i = p + 16 * 7 - 1; sr = 0;
        } else if ((mark == 0xFB || mark == 0xF8) && have_id && p - id_end < 16 * 64) {
            int size = 128 << (id[4] & 3);
            uint8_t *rec = malloc(size + 3);
            if (mfm_bytes(cells, ncells, p, rec, size + 3)) {
                struct sector s = {id[1], id[2], id[3], id[4] & 3, 0, malloc(size)};
                s.ok = crc16(rec, size + 1, crc16(a1, 3, 0xFFFF)) == (rec[size + 1] << 8 | rec[size + 2]);
                memcpy(s.data, rec + 1, size);
                if (id_end <= len * 8) add_sector(cyl, head, s); else free(s.data);
                i = p + 16L * (size + 3) - 1; sr = 0;
            }
            free(rec);
            have_id = 0;
        }
    }
    free(cells);
}

static int load_hfe(const uint8_t *d, long size)
{
    int ntracks = d[9], nsides = d[10], enc = d[11];
    if (enc != 0 && enc != 0xFF) { fprintf(stderr, "floppy: HFE track encoding %d is not MFM\n", enc); return 0; }
    long lut = (d[18] | d[19] << 8) * 512L;
    for (int t = 0; t < ntracks && t < CYLS; t++) {
        if (lut + 4 * t + 4 > size) return 0;
        long off = (d[lut + 4 * t] | d[lut + 4 * t + 1] << 8) * 512L;
        long len = d[lut + 4 * t + 2] | d[lut + 4 * t + 3] << 8;
        if (off + len > size) return 0;
        uint8_t *side[2] = {malloc(len), malloc(len)};
        long n[2] = {0, 0};
        for (long b = 0; b < len; b += 512) {      /* 256 bytes of each side in turn */
            long k = (len - b) / 2 < 256 ? (len - b) / 2 : 256;
            memcpy(side[0] + n[0], d + off + b, k); n[0] += k;
            memcpy(side[1] + n[1], d + off + b + 256, k); n[1] += k;
        }
        for (int s = 0; s < nsides && s < 2; s++) mfm_track(t, s, side[s], n[s]);
        free(side[0]); free(side[1]);
    }
    /* the disc's own geometry: the commonest track layout */
    int best = 0;
    for (int c = 0; c < CYLS; c++)
        for (int h = 0; h < 2; h++) {
            struct track *t = &tracks[c][h];
            if (!t->n) continue;
            int votes = 0, low = 255;
            for (int c2 = 0; c2 < CYLS; c2++)
                for (int h2 = 0; h2 < 2; h2++)
                    votes += tracks[c2][h2].n == t->n && tracks[c2][h2].s[0].n == t->s[0].n;
            for (int k = 0; k < t->n; k++) if (t->s[k].r < low) low = t->s[k].r;
            if (votes > best) {
                best = votes;
                geom.log2size = 7 + t->s[0].n; geom.spt = t->n; geom.low = low;
                geom.heads = nsides > 1 ? 2 : 1;
            }
        }
    return best > 0;
}

/* A dump that stops short by whole tracks (Ego: Repton 4's, 795K: its last
 * track, which its protection needs unreadable, isn't in it) has those
 * tracks unformatted: no sectors on them. */
static int load_flat(const uint8_t *d, long size)
{
    if (size <= 819200 && size % 5120 == 0) { geom.log2size = 10; geom.spt = 5; }       /* ADFS D/E */
    else if (size <= 1638400 && size % 10240 == 0) { geom.log2size = 10; geom.spt = 10; } /* ADFS F */
    else { fprintf(stderr, "floppy: a flat image of %ld bytes: not an 800K or 1600K disc\n", size); return 0; }
    geom.heads = 2; geom.low = 0;
    int secsize = 1 << geom.log2size;
    long ntracks = size / (secsize * geom.spt);
    for (int c = 0; c < 80; c++)
        for (int h = 0; h < 2 && c * 2 + h < ntracks; h++)
            for (int s = 0; s < geom.spt; s++) {
                struct sector x = {c, h, s, geom.log2size - 7, 1, malloc(secsize)};
                memcpy(x.data, d + ((long)(c * 2 + h) * geom.spt + s) * secsize, secsize);
                add_sector(c, h, x);
            }
    return 1;
}

void disc_load(const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "floppy: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size);
    if (fread(d, 1, size, f) != (size_t)size) size = 0;
    fclose(f);
    loaded = size >= 512 && !memcmp(d, "HXCPICFE", 8) ? load_hfe(d, size) : load_flat(d, size);
    free(d);
    if (!loaded) fprintf(stderr, "floppy: %s: no disc read\n", path);
}

/* ---------------------------------------------------------------- ROM modules
 * FileCore and ADFS as OS_Module 18 and OS_FSControl 20 show them: a
 * header, a title, and a SWI handler that issues the SWI asked for
 *     stmfd sp!, {lr}; add pc, pc, r11, lsl #3; nop
 *     then for each SWI n in the chunk: swi X(chunk + n); b exit
 *     exit: ldmfd sp!, {lr}; orrvss pc, lr, #V; bics pc, lr, #V
 * returning as a module's SWI handler does, with the PSR from R14 (the
 * mode and flags the caller's return address carries) and V for an error */
#define ROM_FILECORE  (ROM_MODULES)
#define ROM_ADFS      (ROM_MODULES + 0x400)
#define FILECORE_WORD (ROM_MODULES + 0x800)    /* FileCore's private word */
#define ADFS_INSTANCE (ROM_MODULES + 0x804)    /* ADFS's FileCore instance word */
#define ADFS_WORD     (ROM_MODULES + 0x808)    /* ADFS's private word */

static void rom_module(uint32_t base, const char *title, uint32_t chunk, const char *const *swis)
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
    WR32(base + 0x24, p - base);
    for (const char *const *s = swis; ; s++) {
        size_t l = *s ? strlen(*s) : 0;
        memcpy(MEM + p, *s ? *s : "", l + 1);
        p += l + 1;
        if (!*s) break;
    }
}

void disc_init(void)
{
    rom_module(ROM_FILECORE, "FileCore", 0x40540, (const char *const[]){"FileCore", "DiscOp", NULL});
    rom_module(ROM_ADFS, "ADFS", 0x40240, (const char *const[]){"ADFS", "DiscOp", "HDC", "Drives",
               "FreeSpace", "Retries", NULL});
    WR32(FILECORE_WORD, 0);
    WR32(ADFS_INSTANCE, 0);
    WR32(ADFS_WORD, ADFS_INSTANCE);
    disc_load(rg_floppy);
}

/* OS_Module 18 for the ROM's modules: 1 if name is one */
int disc_rom_module(const char *name, uint32_t *base, uint32_t *word)
{
    if (!strcasecmp(name, "FileCore")) { *base = ROM_FILECORE; *word = FILECORE_WORD; return 1; }
    if (!strcasecmp(name, "ADFS")) { *base = ROM_ADFS; *word = ADFS_WORD; return 1; }
    return 0;
}

/* ---------------------------------------------------------------- DiscOp */
static void disc_error(int num, uint32_t addr)
{
    char msg[64];
    snprintf(msg, sizeof msg, "Disc error %02X at :0/%08X", num, addr & 0x1FFFFFFF);
    os_error(0x108C7, msg);
}

/* R1 reason and flags (bits 8-31: an alternative disc record / 4), R2 the
 * disc address (bits 29-31 the drive), R3 the buffer, R4 the length; on
 * exit R2-R4 say how far it got */
static void disc_op(const char *swi)
{
    uint32_t reason = R[1] & 15, rec = (R[1] >> 8) << 2;
    int log2size = geom.log2size, spt = geom.spt, heads = geom.heads, low = geom.low, seq = 0;
    if (rec) {
        log2size = MEM[rec]; spt = MEM[rec + 1]; heads = MEM[rec + 2];
        low = MEM[rec + 8] & 0x3F; seq = MEM[rec + 8] & 0x40;
    }
    ev(swi, "iiii", R[1], R[2], R[3], R[4]);
    if (reason >= 5 && reason <= 8) return;          /* seek, restore, step: nothing to do */
    if (reason > 1) { os_error(0x108C7, "DiscOp: only reads and verifies on a disc image"); return; }
    if (!loaded || (R[2] >> 29) != 0) { disc_error(0x20, R[2]); return; }   /* (no disc in the drive) */
    if (log2size < 7 || log2size > 10 || !spt || !heads) { disc_error(0x14, R[2]); return; }
    uint32_t secsize = 1u << log2size;
    while (R[4]) {
        uint32_t addr = R[2] & 0x1FFFFFFF, lin = addr >> log2size, t = lin / spt;
        int sec = lin % spt, head = seq ? (int)(t / 80) : (int)(t % heads), cyl = seq ? (int)(t % 80) : (int)(t / heads);
        struct sector *s = NULL;
        if (cyl < CYLS && head < 2)
            for (int k = 0; k < tracks[cyl][head].n; k++)
                if (tracks[cyl][head].s[k].r == low + sec) { s = &tracks[cyl][head].s[k]; break; }
        if (!s) { disc_error(0x14, R[2]); return; }   /* sector not found */
        uint32_t off = addr & (secsize - 1), have = (128u << s->n) > off ? (128u << s->n) - off : 0;
        uint32_t n = secsize - off;
        if (n > R[4]) n = R[4];
        if (n > have) n = have;
        if (!n) { disc_error(0x14, R[2]); return; }
        /* the controller transfers a sector's data as it reads it and finds
         * a bad CRC at the end: the data is in the buffer all the same */
        if (reason == 1) memcpy(MEM + R[3], s->data + off, n);
        if (!s->ok) { disc_error(0x10, R[2]); return; }   /* data CRC */
        R[2] += n; R[3] += n; R[4] -= n;
    }
}

int disc_swi(uint32_t b)
{
    switch (b) {
    case 0x40540: disc_op("FileCore_DiscOp"); return 1;   /* R8: the instance (only ADFS's) */
    case 0x40240: disc_op("ADFS_DiscOp"); return 1;
    case 0x40244: {                                       /* ADFS_Retries: R0 mask, R1 new bits */
        uint32_t old = retries;
        retries = (retries & ~R[0]) | (R[1] & R[0]);
        ev("ADFS_Retries", "ii", R[0], R[1]);
        R[2] = old; R[3] = retries;
        return 1;
    }
    case 0x29:                                            /* OS_FSControl */
        if (R[0] == 14) { ev("OS_FSControl", "ii", R[0], R[1]); return 1; }   /* select a filing system */
        if (R[0] == 20) { R[1] = ROM_ADFS; R[2] = RD32(ADFS_WORD); return 1; }   /* its module base, word */
        return 0;
    }
    return 0;
}
