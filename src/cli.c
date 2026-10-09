/* The command line interpreter (OS_CLI) and system variables.
 *
 * Programs and their loaders run * commands for what !Run scripts do:
 * set and test variables, load and save files, load and kill modules. The
 * line is GSTrans'd first (<name> expands to a variable's value, <nn> to a
 * character; an unset variable expands to nothing). Implemented: Set,
 * SetEval (string values only), Unset, If ... Then ... [Else ...], Load,
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
 * character n, |x -> control character / literal */
void gstrans(const char *in, char *out, int size)
{
    int o = 0;
    while (*in && o < size - 1) {
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
static const char *word_at(const char *s, const char *w)
{
    size_t l = strlen(w);
    return !strncasecmp(s, w, l) && (s[l] == ' ' || s[l] == 0) ? s + l : NULL;
}

static const char *find_word(const char *s, const char *w)
{
    int q = 0;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (!q && s[-1] == ' ' && word_at(s, w)) return s;
    }
    return NULL;
}

/* ---------------------------------------------------------------- commands */
static int arg(const char **s, char *out, int size)
{
    const char *p = skip(*s);
    int o = 0;
    while (*p && *p != ' ' && o < size - 1) out[o++] = *p++;
    out[o] = 0;
    *s = p;
    return o;
}

static int cli_error(int x)
{
    if (x) { VF = 1; return 1; }
    return 0;
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
        if (*p == '"') { char v[256]; long n; int isn; operand(p, v, &n, &isn); roger_setvar(name, v); }
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
        else { uint32_t load, exec; file_info(path, &load, &exec); addr = load; }
        load_file(path, addr);
        const char *leaf = strrchr(name, '.');
        cpu_programs_loaded(leaf ? leaf + 1 : name, addr);
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
    if ((p = word_at(s, "RMLoad")) || (p = word_at(s, "RMRun"))) {
        arg(&p, name, sizeof name);
        if (!modules_rmload(name)) {
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
    /* a command a loaded module provides (*FModeOn...) */
    arg(&s, word, sizeof word);
    if (modules_command(word, skip(s), x)) return 1;
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
    if (!v) { R[2] = 0; if (x) VF = 1; return; }
    if (var_is_number(name) && R[4] != 3) {
        if ((int32_t)R[2] < 0) { R[2] = ~4u; return; }
        if (R[2] < 4) { if (x) VF = 1; return; }
        WR32(R[1], (uint32_t)strtol(v, NULL, 10));
        R[2] = 4; R[3] = R[0]; R[4] = 1;
        return;
    }
    uint32_t l = (uint32_t)strlen(v);
    if ((int32_t)R[2] < 0) { R[2] = ~l; return; }
    if (l > R[2]) { if (x) VF = 1; return; }          /* "Buffer overflow" */
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
