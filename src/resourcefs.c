/* ResourceFS: files that modules hold in their own memory, under
 * Resources:$ (a module's Messages, Toolbox templates and sprites).
 *
 * ResourceFS_RegisterFiles (&41B40) is given a block of files, each:
 *   +0  offset from here to the next file (0: the end of the block)
 *   +4  load address, +8 exec address, +12 length, +16 attributes
 *   +20 its name from Resources:$ ("Resources.Window.Messages"),
 *       zero-terminated and padded to a word
 *   then the length + 4, then the data
 * and ResourceFS_DeregisterFiles (&41B41) the same block again. The files
 * stay in the module's memory, as RISC OS leaves them (the PRMs: the block
 * must stay where it is while registered).
 *
 * Here each registered file is also written to a temporary host directory
 * (with its catalogue information as a ,load-exec suffix), so that every
 * file call reads Resources: files as it does the program's own; and where
 * each lies in memory is kept, for MessageTrans, which uses a ResourceFS
 * file in place rather than copying it. */
#define _GNU_SOURCE                       /* nftw, mkdtemp */
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "internal.h"

static char resdir[256];

#define MAXRES 512
static struct { char name[256]; uint32_t data, len; } res[MAXRES];
static int nres;

static const char *dir(void)
{
    if (!resdir[0]) {
        const char *t = getenv("TMPDIR");
        snprintf(resdir, sizeof resdir, "%s/roger-resourcefs-XXXXXX", t && *t ? t : "/tmp");
        if (!mkdtemp(resdir)) resdir[0] = 0;
    }
    return resdir[0] ? resdir : NULL;
}

/* a Resources: name (Resources:$.a.b, Resources:a.b): the rest, or NULL */
const char *resourcefs_name(const char *n, const char **hostdir)
{
    if (strncasecmp(n, "Resources:", 10) || !dir()) return NULL;
    n += 10;
    if (*n == '$') { n++; if (*n == '.') n++; }
    *hostdir = resdir;
    return n;
}

/* the file's host path: each part of the RISC OS name a directory */
static void host_of(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/", resdir);
    size_t o = strlen(out);
    for (const char *p = name; *p && o + 1 < size; p++) out[o++] = *p == '.' ? '/' : *p == '/' ? '.' : *p;
    out[o] = 0;
}

static int remove_one(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)st; (void)type; (void)f;
    return remove(p);
}
void resourcefs_remove(void)
{
    if (resdir[0]) nftw(resdir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

/* a registered file in memory: 1 and its data and length */
int resourcefs_file(const char *name, uint32_t *data, uint32_t *len)
{
    const char *hd, *n = resourcefs_name(name, &hd);
    if (!n) return 0;
    for (int k = 0; k < nres; k++)
        if (!strcasecmp(res[k].name, n)) { *data = res[k].data; *len = res[k].len; return 1; }
    return 0;
}

static void register_files(uint32_t b, int add)
{
    if (!dir()) return;
    for (uint32_t f = b; RD32(f); f += RD32(f)) {
        char name[256], path[2048];
        cstr(f + 20, name, 0);
        uint32_t load = RD32(f + 4), exec = RD32(f + 8), len = RD32(f + 12);
        uint32_t data = (f + 20 + strlen(name) + 4) & ~3u;      /* past the name: length + 4, data */
        data += 4;
        host_of(name, path, sizeof path);
        if (add) {
            for (char *s = path + strlen(resdir) + 1; (s = strchr(s, '/')); s++) {   /* its directories */
                *s = 0; mkdir(path, 0755); *s = '/';
            }
            char typed[2100];
            snprintf(typed, sizeof typed, "%s,%08x-%08x", path, load, exec);
            FILE *fp = fopen(typed, "wb");
            if (fp) { fwrite(MEM + data, 1, len, fp); fclose(fp); }
            int k;
            for (k = 0; k < nres && strcasecmp(res[k].name, name); k++) ;
            if (k == nres && nres < MAXRES) nres++;
            if (k < MAXRES) { snprintf(res[k].name, sizeof res[k].name, "%s", name); res[k].data = data; res[k].len = len; }
        } else {
            char typed[2100];
            snprintf(typed, sizeof typed, "%s,%08x-%08x", path, load, exec);
            remove(typed);
            for (int k = 0; k < nres; k++)
                if (!strcasecmp(res[k].name, name)) { res[k] = res[--nres]; break; }
        }
        ev(add ? "ResourceFS_Register" : "ResourceFS_Deregister", "s", name, (int)strlen(name));
    }
}

int resourcefs_swi(uint32_t b)
{
    if (b == 0x41B40) { register_files(R[0], 1); return 1; }
    if (b == 0x41B41) { register_files(R[0], 0); return 1; }
    return 0;
}
