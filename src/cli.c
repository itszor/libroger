/* The command line interpreter (OS_CLI) and system variables.
 *
 * Programs and their loaders run * commands for what !Run scripts do:
 * set and test variables, load and save files, load and kill modules. The
 * line is GSTrans'd first (<name> expands to a variable's value, <nn> to a
 * character; an unset variable expands to nothing). Implemented: Set,
 * SetEval (integer and string expressions), Unset, If ... Then ... [Else ...], Dir, Load,
 * Save, RMLoad, RMRun, RMKill, FX, Tuning, and the commands of loaded
 * modules. Anything else is only logged (the OS_CLI event every command
 * writes). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "internal.h"

/* ---------------------------------------------------------------- variables */
#define MAXVAR 64
static struct { char name[64]; char *val; int number; } vars[MAXVAR];   /* number: type 1 */

const char *roger_getvar(const char *name)
{
    for (int k = 0; k < MAXVAR; k++)
        if (vars[k].val && !strcasecmp(vars[k].name, name)) return vars[k].val;
    return NULL;
}

void roger_setvar(const char *name, const char *value)
{
    int k, free_k = -1;
    for (k = 0; k < MAXVAR; k++) {
        if (vars[k].val && !strcasecmp(vars[k].name, name)) break;
        if (!vars[k].val && free_k < 0) free_k = k;
    }
    if (k == MAXVAR) { if (free_k < 0 || !value) return; k = free_k; }
    free(vars[k].val);
    vars[k].val = NULL;
    vars[k].number = 0;
    if (!value) return;
    snprintf(vars[k].name, sizeof vars[k].name, "%s", name);
    vars[k].val = strdup(value);
}

void cli_init(void)
{
    if (rg->variables)
        for (int k = 0; rg->variables[k] && rg->variables[k + 1]; k += 2)
            roger_setvar(rg->variables[k], rg->variables[k + 1]);
}

/* GSTrans: <name> -> the variable's value (nothing if unset), <n> -> the
 * character n, |x -> control character / literal; a control character ends
 * the string, as GSRead has it (Phaethon sets Zone$Dir from a CR-terminated
 * part of a longer buffer, with its length) */
void gstrans(const char *in, char *out, int size)
{
    int o = 0;
    while ((unsigned char)*in >= ' ' && o < size - 1) {
        if (*in == '<') {
            const char *e = strchr(in + 1, '>');
            if (e && e - in - 1 < 64) {
                char name[64];
                memcpy(name, in + 1, e - in - 1);
                name[e - in - 1] = 0;
                char *end;
                long n = strtol(name, &end, 0);
                if (*name && !*end) { out[o++] = (char)n; in = e + 1; continue; }
                if (!strchr(name, ' ')) {
                    const char *v = roger_getvar(name);
                    for (; v && *v && o < size - 1; v++) out[o++] = *v;
                    in = e + 1;
                    continue;
                }
            }
        } else if (*in == '|' && in[1]) {
            char c = in[1];
            out[o++] = c == '|' ? '|' : c == '<' ? '<' : c == '"' ? '"' : c == '?' ? 127 : (char)(c & 31);
            in += 2;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- If */
static const char *skip(const char *s) { while (*s == ' ') s++; return s; }

/* one operand: "string", a number (decimal, &hex) or a bare word */
static const char *operand(const char *s, char *str, long *num, int *isnum)
{
    int o = 0;
    s = skip(s);
    *isnum = 0;
    if (*s == '"') {
        for (s++; *s && *s != '"' && o < 255; ) str[o++] = *s++;
        if (*s == '"') s++;
    } else if (*s == '&' || isdigit((unsigned char)*s) || (*s == '-' && isdigit((unsigned char)s[1]))) {
        char *e;
        *num = *s == '&' ? strtol(s + 1, &e, 16) : strtol(s, &e, 10);
        *isnum = 1;
        s = e;
    } else
        while (*s && *s != ' ' && *s != '=' && *s != '<' && *s != '>' && o < 255) str[o++] = *s++;
    str[o] = 0;
    return s;
}

/* Expressions (OS_EvaluateExpression's, for SetEval): integers (decimal,
 * &hex) and "strings"; ( ), unary + - NOT, * / MOD, + - (+ joins strings),
 * << >>, comparisons = <> < > <= >= (TRUE is -1), AND OR EOR - in rising
 * order of looseness. Bare words are strings. */
struct xv { int num; long n; char s[256]; };
static const char *xp_or(const char *e, struct xv *v, int *ok);
static int xp_word(const char **e, const char *w)
{
    size_t l = strlen(w);
    if (strncasecmp(skip(*e), w, l)) return 0;
    *e = skip(*e) + l;
    return 1;
}
static const char *xp_prim(const char *e, struct xv *v, int *ok)
{
    e = skip(e);
    if (*e == '(') {
        e = xp_or(e + 1, v, ok);
        e = skip(e);
        if (*e == ')') e++; else *ok = 0;
        return e;
    }
    if (*e == '-' || *e == '+') {
        int neg = *e == '-';
        e = xp_prim(e + 1, v, ok);
        if (!v->num) *ok = 0; else if (neg) v->n = -v->n;
        return e;
    }
    if (xp_word(&e, "NOT")) { e = xp_prim(e, v, ok); if (!v->num) *ok = 0; v->n = ~v->n; return e; }
    v->n = 0; v->s[0] = 0;
    e = operand(e, v->s, &v->n, &v->num);
    if (!v->num) {                                      /* a bare word: no operators in it */
        char *o = strpbrk(v->s, "+-*/()");
        if (o && o != v->s) { e -= strlen(o); *o = 0; }
    }
    return e;
}
static const char *xp_mul(const char *e, struct xv *v, int *ok)
{
    e = xp_prim(e, v, ok);
    for (;;) {
        const char *f = skip(e);
        int op = *f == '*' ? '*' : *f == '/' ? '/' : !strncasecmp(f, "MOD", 3) ? 'M' : 0;
        if (!op) return e;
        struct xv w;
        e = xp_prim(f + (op == 'M' ? 3 : 1), &w, ok);
        if (!v->num || !w.num || (op != '*' && !w.n)) { *ok = 0; return e; }
        v->n = op == '*' ? v->n * w.n : op == '/' ? v->n / w.n : v->n % w.n;
    }
}
static const char *xp_add(const char *e, struct xv *v, int *ok)
{
    e = xp_mul(e, v, ok);
    for (;;) {
        const char *f = skip(e);
        if (*f != '+' && *f != '-') return e;
        struct xv w;
        e = xp_mul(f + 1, &w, ok);
        if (v->num && w.num) v->n = *f == '+' ? v->n + w.n : v->n - w.n;
        else if (!v->num && !w.num && *f == '+') {
            size_t l = strlen(v->s), m = strlen(w.s);
            if (l + m > sizeof v->s - 1) m = sizeof v->s - 1 - l;
            memcpy(v->s + l, w.s, m); v->s[l + m] = 0;
        }
        else { *ok = 0; return e; }
    }
}
static const char *xp_shift(const char *e, struct xv *v, int *ok)
{
    e = xp_add(e, v, ok);
    for (;;) {
        const char *f = skip(e);
        if (strncmp(f, "<<", 2) && strncmp(f, ">>", 2)) return e;
        struct xv w;
        e = xp_add(f + 2, &w, ok);
        if (!v->num || !w.num) { *ok = 0; return e; }
        v->n = f[0] == '<' ? (long)((uint32_t)v->n << (w.n & 31)) : (long)((int32_t)v->n >> (w.n & 31));
    }
}
static const char *xp_cmp(const char *e, struct xv *v, int *ok)
{
    e = xp_shift(e, v, ok);
    const char *f = skip(e);
    char op[3] = {0};
    int l = 0;
    while (l < 2 && (f[l] == '=' || f[l] == '<' || f[l] == '>')) { op[l] = f[l]; l++; }
    if (!l || !strcmp(op, "<<") || !strcmp(op, ">>")) return e;
    struct xv w;
    e = xp_shift(f + l, &w, ok);
    if (v->num != w.num) { *ok = 0; return e; }
    int c = v->num ? (v->n > w.n) - (v->n < w.n) : strcmp(v->s, w.s);
    int r = !strcmp(op, "=") ? c == 0 : !strcmp(op, "<>") ? c != 0 : !strcmp(op, "<") ? c < 0
          : !strcmp(op, ">") ? c > 0 : !strcmp(op, "<=") ? c <= 0 : !strcmp(op, ">=") ? c >= 0 : (*ok = 0);
    v->num = 1; v->n = r ? -1 : 0;
    return e;
}
static const char *xp_or(const char *e, struct xv *v, int *ok)
{
    e = xp_cmp(e, v, ok);
    for (;;) {
        int op = xp_word(&e, "AND") ? 'A' : xp_word(&e, "OR") ? 'O' : xp_word(&e, "EOR") ? 'E' : 0;
        if (!op) return e;
        struct xv w;
        e = xp_cmp(e, &w, ok);
        if (!v->num || !w.num) { *ok = 0; return e; }
        v->n = op == 'A' ? (v->n & w.n) : op == 'O' ? (v->n | w.n) : (v->n ^ w.n);
    }
}

static int if_condition(const char *e)
{
    char a[256], b[256], op[3] = {0};
    long na = 0, nb = 0;
    int ia, ib;
    e = operand(e, a, &na, &ia);
    e = skip(e);
    if (!*e) return ia ? na != 0 : 0;
    int l = 0;
    while (l < 2 && (*e == '=' || *e == '<' || *e == '>')) op[l++] = *e++;
    operand(e, b, &nb, &ib);
    int c = ia && ib ? (na > nb) - (na < nb) : strcmp(ia ? "" : a, ib ? "" : b);
    if (!strcmp(op, "=")) return c == 0;
    if (!strcmp(op, "<>")) return c != 0;
    if (!strcmp(op, "<")) return c < 0;
    if (!strcmp(op, ">")) return c > 0;
    if (!strcmp(op, "<=")) return c <= 0;
    if (!strcmp(op, ">=")) return c >= 0;
    return 0;
}

/* the word w (case-insensitive) at s, followed by a space: a pointer past it */
/* s starts with command w (any case), or an abbreviation of it - a
 * prefix ending in '.', as "L." for Load: the first command it abbreviates
 * in the order they're tried (RISC OS's kernel and modules' order) */
static const char *word_at(const char *s, const char *w)
{
    size_t l = strlen(w);
    if (!strncasecmp(s, w, l) && (s[l] == ' ' || s[l] == 0 || s[l] == '"')) return s + l;   /* *Load"file" */
    size_t a = 0;
    while (a < l && s[a] && s[a] != '.' && s[a] != ' ' && tolower((unsigned char)s[a]) == tolower((unsigned char)w[a])) a++;
    return a > 0 && a < l && s[a] == '.' ? s + a + 1 : NULL;
}

static const char *find_word(const char *s, const char *w)
{
    int q = 0;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (!q && s[-1] == ' ' && !strncasecmp(s, w, strlen(w)) && (s[strlen(w)] == ' ' || !s[strlen(w)]))
            return s;
    }
    return NULL;
}

/* ---------------------------------------------------------------- commands */
static int arg(const char **s, char *out, int size)
{
    const char *p = skip(*s);
    int o = 0;
    if (*p == '"') {                             /* "a name", as RISC OS takes one (Saloon Cars
                                                    Deluxe: *Load "<SALOONC$Dir>.x" addr) */
        for (p++; *p && *p != '"' && o < size - 1; ) out[o++] = *p++;
        if (*p == '"') p++;
        out[o] = 0;
        *s = p;
        return o;
    }
    while (*p && *p != ' ' && o < size - 1) out[o++] = *p++;
    out[o] = 0;
    *s = p;
    return o;
}

/* send a file's bytes to the VDU stream (as OS_WriteC); 0 if it can't be read */
static int print_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int c;
    while ((c = fgetc(f)) != EOF) vdu_put((uint8_t)c);
    fclose(f);
    return 1;
}

static int cli_error(int x)
{
    if (x) { VF = 1; return 1; }
    return 0;
}

int cli_command(const char *line, int x);

/* An alias (Alias$...) run with its parameters, as the CLI expands one:
 * %0-%9 the parameters, %*N the rest of the line from parameter N, each
 * line of the value (*Set made |M a CR) a command of its own */
static int run_alias(const char *alias, const char *params, int x)
{
    const char *start[11];
    int n = 0;
    for (const char *q = skip(params); *q && n < 10; q = skip(q)) {
        start[n++] = q;
        while (*q && *q != ' ') q++;
    }
    start[n] = params + strlen(params);
    char cmd[1100];
    int o = 0, ok = 1;
    for (const char *a = alias; ; a++) {
        if (!*a || *a == '\r' || *a == '\n') {
            cmd[o] = 0;
            if (*skip(cmd) && !cli_command(cmd, x)) ok = 0;
            o = 0;
            if (!*a) break;
            continue;
        }
        const char *from = NULL, *to = NULL;
        if (a[0] == '%' && a[1] == '*' && a[2] >= '0' && a[2] <= '9') {
            int k = a[2] - '0';
            if (k < n) { from = start[k]; to = start[n]; }
            else from = to = "";
            a += 2;
        } else if (a[0] == '%' && a[1] >= '0' && a[1] <= '9') {
            int k = a[1] - '0';
            if (k < n) { from = start[k]; for (to = from; *to && *to != ' '; to++) ; }
            else from = to = "";
            a += 1;
        }
        if (from) {
            while (to > from && to[-1] == ' ') to--;
            while (from < to && o < (int)sizeof cmd - 1) cmd[o++] = *from++;
        } else if (o < (int)sizeof cmd - 1) cmd[o++] = *a;
    }
    return ok;
}

/* run one command line; 0 = not understood (only logged) */
int cli_command(const char *line, int x)
{
    char buf[1100], word[1100], name[1100];
    gstrans(line, buf, sizeof buf);
    const char *s = skip(buf), *p;
    while (*s == '*' || *s == ' ') s++;
    if (*s == '|' || !*s) return 1;                   /* comment */
    if ((p = word_at(s, "Set"))) {
        arg(&p, name, sizeof name);
        roger_setvar(name, skip(p));
        return 1;
    }
    if ((p = word_at(s, "SetEval")) && (p = skip(p))) {
        arg(&p, name, sizeof name);
        p = skip(p);
        struct xv v = {0};
        int ok = 1;
        const char *end = skip(xp_or(p, &v, &ok));
        if (!ok || *end) { ev("SetEval_bad_expression", "s", p, (int)strlen(p)); return cli_error(x) || 1; }
        if (v.num) {
            char t[24];
            snprintf(t, sizeof t, "%ld", (long)(int32_t)v.n);
            roger_setvar(name, t);
            for (int k = 0; k < MAXVAR; k++)
                if (vars[k].val && !strcasecmp(vars[k].name, name)) vars[k].number = 1;
        } else roger_setvar(name, v.s);
        return 1;
    }
    if ((p = word_at(s, "Unset"))) { arg(&p, name, sizeof name); roger_setvar(name, NULL); return 1; }
    if ((p = word_at(s, "If"))) {
        const char *then = find_word(p, "Then");
        if (!then) return cli_error(x);
        char cond[1100];
        snprintf(cond, sizeof cond, "%.*s", (int)(then - p), p);
        const char *els = find_word(then + 4, "Else");
        char cmd[1100];
        if (if_condition(cond)) snprintf(cmd, sizeof cmd, "%.*s", els ? (int)(els - then - 4) : 1099, then + 4);
        else if (els) snprintf(cmd, sizeof cmd, "%s", els + 4);
        else return 1;
        return cli_command(cmd, x);
    }
    if ((p = word_at(s, "Dir"))) {               /* the current directory */
        arg(&p, name, sizeof name);
        if (!set_csd(name)) {
            fprintf(stderr, "*Dir: not found %s\n", name);
            if (cli_error(x)) return 1;
            finish("directory not found");
        }
        return 1;
    }
    if ((p = word_at(s, "Load"))) {
        char path[2048];
        arg(&p, name, sizeof name);
        if (!hostpath_read(name, path)) {
            fprintf(stderr, "*Load: file not found %s\n", name);
            if (cli_error(x)) return 1;
            finish("file not found");
        }
        arg(&p, word, sizeof word);
        uint32_t addr;
        if (*word) addr = (uint32_t)strtoul(word, NULL, 16);
        else {                                   /* its load address; an Absolute (&FF8) at &8000, */
            uint32_t load, exec;                 /* as Alias$@LoadType_FF8 (Load %*0 8000) has it */
            file_info(path, &load, &exec);       /* (Phaethon's loader *Loads !RunImage so) */
            addr = load >> 8 == 0xFFFFF8 ? 0x8000 : load;
        }
        load_file(path, addr);
        cpu_programs_loaded(name, addr);
        return 1;
    }
    if ((p = word_at(s, "EnumDir"))) {           /* a directory's leaves, a line each, into a file
                                                    (Saloon Cars Deluxe's list of players) */
        static struct dir_entry e[256];
        char out[1100];
        arg(&p, name, sizeof name);
        arg(&p, out, sizeof out);
        int n = dir_entries(name, e, 256);
        if (n < 0 || !*out) {
            fprintf(stderr, "*EnumDir: directory not found %s\n", name);
            if (cli_error(x)) return 1;
            finish("directory not found");
        }
        uint32_t r[8];                           /* written as the program would: OS_Find, OS_BPut */
        int sx = swi_x, vf = VF;
        memcpy(r, R, sizeof r);
        R[0] = 0x83; R[1] = roger_scratch(out);
        roger_swi(0x2000D);                      /* XOS_Find: open out */
        uint32_t h = VF ? 0 : R[0];
        for (int k = 0; h && k < n; k++)
            for (const char *c = e[k].leaf; ; c++) {
                R[0] = *c ? (uint8_t)*c : 10; R[1] = h;
                roger_swi(0x2000B);              /* XOS_BPut */
                if (!*c) break;
            }
        if (h) { R[0] = 0; R[1] = h; roger_swi(0x2000D); }
        memcpy(R, r, sizeof r);
        swi_x = sx; VF = vf;
        if (!h) {
            fprintf(stderr, "*EnumDir: can't write %s\n", out);
            return cli_error(x) || 1;
        }
        return 1;
    }
    if ((p = word_at(s, "SLoad")) || (p = word_at(s, "SMerge"))) {   /* the system sprite area */
        int merge = !strncasecmp(s, "SMerge", 6);
        arg(&p, name, sizeof name);
        if (!sprite_file(sys_sprites(), name, merge)) {
            fprintf(stderr, "*%s: file not found %s\n", merge ? "SMerge" : "SLoad", name);
            if (cli_error(x)) return 1;
            finish("file not found");
        }
        return 1;
    }
    if ((p = word_at(s, "ChannelVoice"))) {    /* SoundChannels: a voice for a channel */
        arg(&p, word, sizeof word);
        arg(&p, name, sizeof name);
        if (!sound_channel_voice(atoi(word), name)) {
            ev("ChannelVoice_unknown", "s", name, (int)strlen(name));
            if (cli_error(x)) return 1;
        }
        return 1;
    }
    if ((p = word_at(s, "IconSprites"))) {     /* sprites for the Wimp's pool */
        arg(&p, name, sizeof name);
        if (!sprite_file(wimp_sprites(), name, 1)) {
            ev("IconSprites_missing", "s", name, (int)strlen(name));
            if (cli_error(x)) return 1;
        }
        return 1;
    }
    if ((p = word_at(s, "ScreenLoad"))) {       /* a sprite file's first sprite to the screen */
        arg(&p, name, sizeof name);
        if (!screen_load(name)) {
            fprintf(stderr, "*ScreenLoad: file not found %s\n", name);
            if (cli_error(x)) return 1;
            finish("file not found");
        }
        return 1;
    }
    if ((p = word_at(s, "SChoose"))) {          /* a system sprite, for PLOT &E8-&EF */
        arg(&p, name, sizeof name);
        if (!vdu_schoose(name)) ev("SChoose_missing", "s", name, (int)strlen(name));
        return 1;
    }
    if ((p = word_at(s, "Save"))) {
        arg(&p, name, sizeof name);
        arg(&p, word, sizeof word);
        char *e;
        uint32_t start = (uint32_t)strtoul(word, &e, 16), end;
        if (*e == '+') end = start + (uint32_t)strtoul(e + 1, NULL, 16);   /* start+length */
        else {
            arg(&p, word, sizeof word);
            if (*word == '+') end = start + (uint32_t)strtoul(word + 1, NULL, 16);
            else end = (uint32_t)strtoul(word, NULL, 16);
        }
        if (end < start || end - start > MEMSIZE - start) return cli_error(x);
        save_file(name, start, end - start);
        return 1;
    }
    if ((p = word_at(s, "RMEnsure"))) {
        /* *RMEnsure title version [command]: the command (or an error, if
         * there's none) unless the module is there at that version or
         * later. RISC OS's own modules are as libroger stands in for them */
        static const struct { const char *title; int version; } os[] = {   /* (without headers) */
            {"FontManager", 287}, {"SoundDMA", 125}, {"SoundChannels", 127}, {"SoundScheduler", 113},
            {"FileCore", 274}, {"ADFS", 299}, {NULL, 0}};
        char ver[64];
        arg(&p, name, sizeof name);
        arg(&p, ver, sizeof ver);
        int have = modules_version(name);
        for (int k = 0; have <= 0 && os[k].title; k++) if (!strcasecmp(os[k].title, name)) have = os[k].version;
        if (have >= version_hundredths(ver)) return 1;
        p = skip(p);
        if (*p) return cli_command(p, x) || 1;
        if (x) os_error(0x107, have < 0 ? "Module not found" : "Module version too old");
        return 1;
    }
    if ((p = word_at(s, "RMLoad")) || (p = word_at(s, "RMRun"))) {
        arg(&p, name, sizeof name);
        if (!modules_rmload(name, skip(p))) {
            fprintf(stderr, "*RMLoad: module file not configured %s\n", name);
            return cli_error(x);
        }
        return 1;
    }
    if ((p = word_at(s, "RMKill"))) {
        arg(&p, name, sizeof name);
        if (!modules_rmkill(name)) return cli_error(x);   /* "Module not found" */
        return 1;
    }
    if (word_at(s, "RMClear")) {
        modules_rmclear();
        return 1;
    }
    if ((p = word_at(s, "Tuning"))) {             /* relative to the current tuning; 0: the default */
        p = skip(p);
        int neg = *p == '-';
        if (*p == '-' || *p == '+') p++;
        long n = *p == '&' ? strtol(p + 1, NULL, 16) : strtol(p, NULL, 10);
        uint32_t saved = R[0];
        R[0] = 0;
        swi(0x40187, swi_pc);                         /* Sound_Tuning: read */
        R[0] = n ? (uint32_t)((int32_t)R[0] + (neg ? -n : n)) & 0xFFFF : 0x6AB0;
        swi(0x40187, swi_pc);
        R[0] = saved;
        return 1;
    }
    if ((p = word_at(s, "FX"))) {
        uint32_t v[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            p = skip(p);
            if (*p == ',') p = skip(p + 1);
            if (!*p) break;
            char *e;
            v[k] = (uint32_t)strtoul(*p == '&' ? p + 1 : p, &e, *p == '&' ? 16 : 10);
            p = e;
        }
        uint32_t saved[3] = {R[0], R[1], R[2]};
        R[0] = v[0]; R[1] = v[1]; R[2] = v[2];
        swi(6, swi_pc);
        R[0] = saved[0]; R[1] = saved[1]; R[2] = saved[2];
        return 1;
    }
    if ((p = word_at(s, "Copy"))) {             /* *Copy from to [options]: R recurses */
        char to[1100], opts[256] = "";
        arg(&p, name, sizeof name);
        arg(&p, to, sizeof to);
        while (*skip(p)) { arg(&p, word, sizeof word); strncat(opts, word, sizeof opts - strlen(opts) - 1); }
        int recurse = 0;
        for (const char *o = opts; *o; o++)
            if ((*o == 'R' || *o == 'r') && (o == opts || o[-1] != '~')) recurse = 1;
        if (!copy_object(name, to, recurse)) {
            fprintf(stderr, "*Copy: can't copy %s to %s\n", name, to);
            return cli_error(x) || 1;
        }
        return 1;
    }
    if ((p = word_at(s, "Delete")) || (p = word_at(s, "Remove"))) {
        arg(&p, name, sizeof name);
        if (!delete_object(name) && !strncasecmp(s, "Delete", 6)) return cli_error(x) || 1;
        return 1;
    }
    if ((p = word_at(s, "Filer_OpenDir")) || (p = word_at(s, "Filer_CloseDir"))) {   /* a directory */
        arg(&p, name, sizeof name);                  /* viewer (on the desktop: filer.c) */
        if (!strncasecmp(s, "Filer_O", 7)) filer_open(name);
        else filer_close(name);
        return 1;
    }
    if ((p = word_at(s, "Print"))) {             /* a file's bytes, to the VDU drivers */
        char path[2048];
        arg(&p, name, sizeof name);
        if (!hostpath_read(name, path) || !print_file(path)) {
            fprintf(stderr, "*Print: file not found %s\n", name);
            return cli_error(x);
        }
        return 1;
    }
    /* a command a loaded module provides (*FModeOn...) */
    const char *cmd = s;
    arg(&s, word, sizeof word);
    if (modules_command(word, skip(s), x)) return 1;
    /* running a file (*Run name, or the name as the command, with or
     * without a leading slash): a BBC font file (&FF7) is printed, as
     * Alias$@RunType_FF7 (Print %*0) does, and a module (&FFA) RMRun
     * (Alias$@RunType_FFA) */
    if ((p = word_at(cmd, "Run"))) arg(&p, word, sizeof word);
    {
        char path[2048];
        const char *f = word[0] == '/' ? word + 1 : word;
        uint32_t load, exec;
        if (*f && hostpath_read(f, path)) {
            file_info(path, &load, &exec);
            if ((load >> 20) == 0xFFF && ((load >> 8) & 0xFFF) == 0xFF7) return print_file(path);
            if ((load >> 20) == 0xFFF && ((load >> 8) & 0xFFF) == 0xFFA) {
                if (modules_rmload(f, skip(p ? p : s))) return 1;
                fprintf(stderr, "*RMRun: module file not configured %s\n", f);
                return cli_error(x);
            }
            /* another type: its run type, Alias$@RunType_TTT (e.g. a music
             * file a module sets one for: MusicModule4's &CC5, *MusLoad),
             * with the file's name and the rest of the line as parameters */
            if ((load >> 20) == 0xFFF) {
                char var[32], params[1100];
                snprintf(var, sizeof var, "Alias$@RunType_%03X", (unsigned)(load >> 8) & 0xFFF);
                const char *alias = roger_getvar(var);
                if (alias) {
                    snprintf(params, sizeof params, "%s %s", f, skip(p ? p : s));
                    return run_alias(alias, params, x);
                }
            }
        }
    }
    return 0;
}

void os_cli(int x)
{
    char s[1100];
    int l = cstr(R[0], s, 1);
    ev("OS_CLI", "s", s, l);
    if (!strncasecmp(s, "QUIT", 4)) finish("OS_CLI QUIT");
    uint32_t r0 = R[0];
    cli_command(s, x);
    R[0] = r0;
}

static int var_is_number(const char *name)
{
    for (int k = 0; k < MAXVAR; k++)
        if (vars[k].val && !strcasecmp(vars[k].name, name)) return vars[k].number;
    return 0;
}

/* OS_ReadVarVal: R0 name, R1 buffer, R2 size (negative: does it exist?),
 * R3 context (0: first), R4 type wanted (3: an expanded string) -> R2
 * length, R3 name, R4 type: 0 string, or 1 a number (4 bytes, unless a
 * string was asked for) */
static void read_var(int x)
{
    char name[256];
    cstr(R[0], name, 1);
    const char *v = R[3] ? NULL : roger_getvar(name);
    if (!v) { R[2] = 0; if (x) os_error(0x124, "System variable not found"); return; }
    if (var_is_number(name) && R[4] != 3) {
        if ((int32_t)R[2] < 0) { R[2] = ~4u; return; }
        if (R[2] < 4) { if (x) VF = 1; return; }
        WR32(R[1], (uint32_t)strtol(v, NULL, 10));
        R[2] = 4; R[3] = R[0]; R[4] = 1;
        return;
    }
    uint32_t l = (uint32_t)strlen(v);
    if ((int32_t)R[2] < 0) { R[2] = ~l; return; }
    if (l > R[2]) { if (x) os_error(0x1E4, "Buffer overflow"); return; }
    memcpy(MEM + R[1], v, l);
    R[2] = l; R[3] = R[0]; R[4] = 0;
}

/* OS_SetVarVal: R0 name, R1 value, R2 length (negative: delete), R4 type
 * (0 string, GSTrans'd; 1 number, a 4-byte integer, kept as a number;
 * 2 macro, 3 expression - taken as strings) */
static void set_var(void)
{
    char name[256], val[1100];
    cstr(R[0], name, 1);
    if ((int32_t)R[2] < 0) { roger_setvar(name, NULL); return; }
    if (R[4] == 1) snprintf(val, sizeof val, "%d", (int32_t)RD32(R[1]));
    else {
        uint32_t l = R[2] < 1099 ? R[2] : 1099;
        memcpy(val, MEM + R[1], l);
        val[l] = 0;
        if (R[4] == 0) { char t[1100]; gstrans(val, t, sizeof t); strcpy(val, t); }
    }
    roger_setvar(name, val);
    if (R[4] == 1)
        for (int k = 0; k < MAXVAR; k++)
            if (vars[k].val && !strcasecmp(vars[k].name, name)) vars[k].number = 1;
}

int cli_swi(uint32_t b, int x)
{
    switch (b) {
    case 0x05: os_cli(x); return 1;
    case 0x23: read_var(x); return 1;
    case 0x24: set_var(); return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- for game code */
void roger_swi(uint32_t n) { swi(n, swi_pc); }

void roger_cli(const char *command)
{
    R[0] = roger_scratch(command);
    swi(5, swi_pc);
}

uint32_t roger_scratch(const char *s)
{
    size_t l = strlen(s);
    if (l > 1023) l = 1023;
    memcpy(MEM + SCRATCH, s, l);
    MEM[SCRATCH + l] = 0;
    return SCRATCH;
}
