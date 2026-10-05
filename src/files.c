/* Files: RISC OS filenames on the host, OS_File, open files (OS_Find,
 * OS_GBPB, OS_Args, OS_BGet, OS_BPut) and sprite files.
 *
 * The program's directory is roger_config.appdir on the host, holding the
 * files as extracted from the disc: a RISC OS name a.b.c is the host path
 * appdir/a/b/c, matched case-insensitively, where a host name may carry the
 * file's catalogue information after a comma - ",xxx" (filetype) or
 * ",llllllll-eeeeeeee" (load and exec addresses) - and RISC OS "/" is ".".
 * A <name> in a filename expands to the system variable's value (after any
 * dir_prefixes are stripped). Files the program writes (open for output or
 * update, OS_File save, *Save) go to savedir, mirroring their path, so the
 * extracted originals stay untouched; reads prefer a saved copy when there
 * is one. */
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "internal.h"

int swi_x;

int cstr(uint32_t a, char *out, int term_ctrl)
{
    int n = 0;
    while (1) {
        uint8_t c = MEM[a];
        if (c == 0 || (term_ctrl && (c == 10 || c == 13))) break;
        out[n++] = c; a++;
        if (n > 1024) break;
    }
    out[n] = 0;
    return n;
}

static const char *strip_prefixes(const char *n)
{
    while (*n == ' ') n++;
    static const char *const discs[] = {"Disc1:", "Disc2:", NULL};
    for (int k = 0; discs[k]; k++)
        if (!strncmp(n, discs[k], strlen(discs[k]))) n += strlen(discs[k]);
    if (rg->dir_prefixes)
        for (int k = 0; rg->dir_prefixes[k]; k++)
            if (!strncasecmp(n, rg->dir_prefixes[k], strlen(rg->dir_prefixes[k]))) n += strlen(rg->dir_prefixes[k]);
    return n;
}

int hostpath(const char *name, char *out)
{
    char x[1100];
    gstrans(strip_prefixes(name), x, sizeof x);
    const char *n = x;
    strcpy(out, rg_appdir);
    char part[256];
    while (*n) {
        int l = 0;
        while (*n && *n != '.' && l < 255) part[l++] = *n++;
        part[l] = 0;
        if (*n == '.') n++;
        for (int k = 0; k < l; k++) if (part[k] == '/') part[k] = '.';
        DIR *d = opendir(out);
        if (!d) return 0;
        struct dirent *e;
        int hit = 0;
        while ((e = readdir(d))) {
            char base[256];
            snprintf(base, sizeof base, "%s", e->d_name);
            char *comma = strchr(base, ',');
            if (comma) *comma = 0;
            if (!strcasecmp(base, part)) { strcat(out, "/"); strcat(out, e->d_name); hit = 1; break; }
        }
        closedir(d);
        if (!hit) return 0;
    }
    return 1;
}
int roger_hostpath(const char *name, char *out) { return hostpath(name, out); }

long load_file(const char *path, uint32_t addr)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = fread(MEM + addr, 1, MEMSIZE - addr, f);
    fclose(f);
    return n;
}
long roger_load_file(const char *path, uint32_t addr) { return load_file(path, addr); }

/* catalogue information from a host name's suffix */
void file_info(const char *path, uint32_t *load, uint32_t *exec)
{
    *load = 0xFFFFFD00; *exec = 0;                    /* untyped: Data */
    const char *comma = strrchr(path, ',');
    if (!comma || strchr(comma, '/')) return;
    unsigned a, b;
    if (sscanf(comma + 1, "%x-%x", &a, &b) == 2) { *load = a; *exec = b; }
    else if (strlen(comma + 1) == 3 && sscanf(comma + 1, "%3x", &a) == 1) *load = 0xFFF00000u | a << 8;
}

static void save_path(const char *name, char *out, int create);

/* a file to read: the saved copy if the program has written one */
int hostpath_read(const char *name, char *out)
{
    struct stat st;
    save_path(name, out, 0);
    if (stat(out, &st) == 0) return 1;
    return hostpath(name, out);
}

/* write memory to a file (in savedir) */
int save_file(const char *name, uint32_t addr, uint32_t len)
{
    char spath[2048];
    save_path(name, spath, 1);
    FILE *f = fopen(spath, "wb");
    if (!f) return 0;
    fwrite(MEM + addr, 1, len, f);
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------- OS_File */
static void os_file(void)
{
    uint32_t op = R[0] & 0xFF;
    char name[1100], path[2048];
    int n = cstr(R[1], name, 1);
    ev("OS_File", "is", op, name, n);
    if (op == 0 || op == 10) {                                    /* save (with load/exec, filetype) */
        if (!save_file(name, R[4], R[5] - R[4]) && swi_x) VF = 1;   /* R4 start, R5 end */
        return;
    }
    int ok = hostpath_read(name, path);
    if (op == 255 || op == 12 || op == 14 || op == 16) {          /* load */
        if (!ok) {
            /* an X-flagged (error-checking) caller handles a missing file
             * itself: give it the error */
            if (swi_x) { VF = 1; return; }
            fprintf(stderr, "file not found %s (pc=%08x r1=%08x icount=%llu)\n",
                    name, g_cur_pc, R[1], (unsigned long long)icount);
            finish("file not found");
        }
        if (R[3] & 0xFF) finish("load at file address unsupported");
        long len = load_file(path, R[2]);
        const char *leaf = strrchr(name, '.');
        cpu_programs_loaded(leaf ? leaf + 1 : name, R[2]);
        R[4] = (uint32_t)len; R[0] = 1;
        return;
    }
    if (op == 5 || op == 17 || op == 13 || op == 15) {            /* read catalogue information */
        if (!ok) { R[0] = 0; return; }
        struct stat st;
        stat(path, &st);
        uint32_t load, exec;
        file_info(path, &load, &exec);
        R[0] = 1; R[2] = load; R[3] = exec; R[4] = (uint32_t)st.st_size; R[5] = 3;
        return;
    }
    finish("OS_File op unsupported");
}

/* ---------------------------------------------------------------- open files */
#define NHANDLES 32
static FILE *handles[NHANDLES];           /* RISC OS handle k+1 */

/* where the program's copy of a file goes; create: make its directories */
static void save_path(const char *name, char *out, int create)
{
    char x[1100];
    gstrans(strip_prefixes(name), x, sizeof x);
    const char *n = x;
    snprintf(out, 2048, "%s/", rg_savedir);
    if (create) mkdir(rg_savedir, 0777);
    size_t o = strlen(out);
    for (; *n && o < 2040; n++) {
        if (*n == '.') { out[o] = 0; if (create) mkdir(out, 0777); out[o++] = '/'; }
        else out[o++] = *n == '/' ? '.' : *n;
    }
    out[o] = 0;
}

static uint32_t open_handle(uint32_t reason, const char *name)
{
    char path[2048], spath[2048];
    int k;
    for (k = 0; k < NHANDLES && handles[k]; k++) ;
    if (k == NHANDLES) return 0;
    save_path(name, spath, (reason & 0xC0) != 0x40);
    FILE *f = NULL;
    struct stat st;
    int saved = stat(spath, &st) == 0;
    if ((reason & 0xC0) == 0x40) {                         /* open for input */
        if (saved) f = fopen(spath, "rb");
        else if (hostpath(name, path)) f = fopen(path, "rb");
    } else if ((reason & 0xC0) == 0x80) {                  /* create for output */
        f = fopen(spath, "w+b");
    } else {                                               /* open for update */
        if (!saved && hostpath(name, path)) {             /* copy the original first */
            FILE *in = fopen(path, "rb"), *out = fopen(spath, "wb");
            if (in && out) { char b[4096]; size_t m; while ((m = fread(b, 1, sizeof b, in))) fwrite(b, 1, m, out); }
            if (in) fclose(in);
            if (out) fclose(out);
        }
        f = fopen(spath, "r+b");
    }
    if (!f) return 0;
    handles[k] = f;
    return k + 1;
}

static FILE *handle(uint32_t h) { return h >= 1 && h <= NHANDLES ? handles[h - 1] : NULL; }

static void os_find(void)
{
    char name[1100];
    if ((R[0] & 0xC0) == 0) {                              /* close (0: all) */
        ev("OS_Find", "ii", R[0], R[1]);
        for (int k = 0; k < NHANDLES; k++)
            if (handles[k] && (R[1] == 0 || R[1] == (uint32_t)k + 1)) { fclose(handles[k]); handles[k] = NULL; }
        return;
    }
    int n = cstr(R[1], name, 1);
    R[0] = open_handle(R[0], name);
    ev("OS_Find", "si", name, n, R[0]);
    if (!R[0] && swi_x) VF = 1;                            /* not found: 0, or an error if asked */
}

/* OS_GBPB 9/10/12: read a directory's entries. R1 directory, R2 buffer,
 * R3 entries wanted, R4 offset (0 first), R5 buffer size, R6 wildcarded
 * name (0: all). Entries come in RISC OS's order (names compared without
 * case), without the host's ",xxx" suffixes. Returns R3 entries read, R4
 * the next offset (-1 at the end). */
static int wild(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') return wild(p + 1, s) || (*s && wild(p, s + 1));
    return *s && (*p == '#' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) && wild(p + 1, s + 1);
}
static int name_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void gbpb_dir(uint32_t op)
{
    char name[1100], dir[2048], pat[256] = "*";
    cstr(R[1], name, 1);
    if (R[6]) cstr(R[6], pat, 1);
    ev("OS_GBPB", "is", op, name, (int)strlen(name));
    if (!hostpath(name, dir)) { R[3] = 0; R[4] = 0xFFFFFFFF; return; }
    DIR *d = opendir(dir);
    char *names[256]; int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 256) {
        if (e->d_name[0] == '.') continue;
        names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    qsort(names, n, sizeof *names, name_cmp);
    uint32_t want = R[3], off = R[4], out = R[2], end = R[2] + R[5], got = 0;
    int k;
    for (k = 0; k < n && got < want; k++) {
        char leaf[256], path[2400];
        snprintf(leaf, sizeof leaf, "%s", names[k]);
        char *comma = strchr(leaf, ',');
        if (comma) *comma = 0;
        for (char *c = leaf; *c; c++) if (*c == '.') *c = '/';
        if ((uint32_t)k < off || !wild(pat, leaf)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, names[k]);
        struct stat st;
        int isdir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        uint32_t len = strlen(leaf) + 1, rec = op == 9 ? len : ((op == 10 ? 20 : 29) + len + 3) & ~3u;
        if (out + rec > end) break;
        if (op != 9) {
            uint32_t load, exec;
            file_info(names[k], &load, &exec);
            if (isdir) { load = 0xFFFFFD00; exec = 0; }
            WR32(out, load); WR32(out + 4, exec); WR32(out + 8, isdir ? 0 : (uint32_t)st.st_size);
            WR32(out + 12, 3); WR32(out + 16, isdir ? 2 : 1);                    /* attributes WR, type */
            if (op == 12) { WR32(out + 20, 0); MEM[out + 24] = 0; memset(MEM + out + 25, 0, 4); }
            memcpy(MEM + out + (op == 10 ? 20 : 29), leaf, len);
        } else memcpy(MEM + out, leaf, len);
        out += rec; got++;
    }
    while (k < n && (uint32_t)k < off) k++;
    R[3] = got; R[4] = k >= n ? 0xFFFFFFFF : (uint32_t)k;
    for (int j = 0; j < n; j++) free(names[j]);
}

static void os_gbpb(void)
{
    FILE *f = handle(R[1]);
    uint32_t op = R[0];
    if (op == 9 || op == 10 || op == 12) { gbpb_dir(op); return; }
    if (!f || op < 1 || op > 4) { ev("OS_GBPB", "ii", op, R[1]); if (swi_x) VF = 1; return; }
    if (op == 1 || op == 3) fseek(f, R[4], SEEK_SET);
    size_t n = op <= 2 ? fwrite(MEM + R[2], 1, R[3], f) : fread(MEM + R[2], 1, R[3], f);
    R[2] += n; R[3] -= n; R[4] = (uint32_t)ftell(f);
    CF = R[3] != 0;                                         /* not all transferred */
}

static void os_args(void)
{
    FILE *f = handle(R[1]);
    if (!f) { if (R[0] == 0 && R[1] == 0) R[0] = 0; return; }   /* r1=0: filing system queries */
    long cur = ftell(f);
    switch (R[0]) {
    case 0: R[2] = (uint32_t)cur; break;                    /* read pointer */
    case 1: fseek(f, R[2], SEEK_SET); break;                /* write pointer */
    case 2: case 4: fseek(f, 0, SEEK_END); R[2] = (uint32_t)ftell(f); fseek(f, cur, SEEK_SET); break;   /* extent */
    case 5: fseek(f, 0, SEEK_END); R[2] = cur >= ftell(f) ? 1 : 0; fseek(f, cur, SEEK_SET); break;      /* EOF? */
    case 255: fflush(f); break;
    default: ev("OS_Args", "ii", R[0], R[1]); break;
    }
}

/* ---------------------------------------------------------------- sprites */
void sprite_op(void)
{
    uint32_t op = R[0] & 0xFF;
    ev("OS_SpriteOp", "iii", R[0], R[1], R[2]);
    if (op == 8 && (R[0] & 0x300) != 0) {         /* read area control block (user area) */
        R[2] = RD32(R[1]); R[3] = RD32(R[1] + 4); R[4] = RD32(R[1] + 8); R[5] = RD32(R[1] + 12);
        return;
    }
    if (rg_full_os && sprite_op_full()) return;
    if (op == 10 || op == 11) {                   /* load / merge sprite file */
        char name[1100], path[2048];
        cstr(R[2], name, 1);
        if (!hostpath(name, path)) finish("sprite file not found");
        if (op == 10) load_file(path, R[1] + 4);
    }
}

int files_swi(uint32_t b)
{
    switch (b) {
    case 0x08: os_file(); return 1;
    case 0x0D: os_find(); return 1;
    case 0x0C: os_gbpb(); return 1;
    case 0x09: os_args(); return 1;
    case 0x0A: { FILE *f = handle(R[1]); int c = f ? fgetc(f) : EOF;     /* OS_BGet */
                 CF = c == EOF; R[0] = c == EOF ? 0 : (uint32_t)c; return 1; }
    case 0x0B: { FILE *f = handle(R[1]); if (f) fputc(R[0] & 0xFF, f); return 1; }   /* OS_BPut */
    case 0x2E: sprite_op(); return 1;
    }
    return 0;
}
