/* Start-up, the command line, logging, scripted input and finishing.
 *
 * Every run writes <out>.events (the SWIs and events the program made, one
 * per line with the instruction count), and at the end <out>.vdu (the VDU
 * stream), <out>.screen (screen memory) and <out>.regs (registers): in a
 * headless run (--limit=N) all of it is deterministic, so two builds can be
 * compared byte for byte. */
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "internal.h"

const struct roger_config *rg;
int rg_interactive, rg_full_os, rg_trace_gaps;
uint64_t rg_limit = 20000000, rg_t0_ns;
const char *rg_appdir, *rg_savedir = "savedata", *rg_floppy;
static const char *outprefix = "recomp";
static FILE *evlog;
extern volatile uint64_t rg_last_service_ns;
void irq_start_clock(uint64_t now);

int roger_interactive(void) { return rg_interactive; }

/* ---------------------------------------------------------------- the event log */
static void vev(const char *name, const char *fmt, va_list ap)
{
    fprintf(evlog, "%llu %s", (unsigned long long)icount, name);
    for (const char *p = fmt; *p; p++) {
        if (*p == 'i') fprintf(evlog, " %x", va_arg(ap, unsigned));
        else if (*p == 's') {                  /* bytes: pointer + length */
            const uint8_t *s = va_arg(ap, const uint8_t *);
            int n = va_arg(ap, int);
            fputc(' ', evlog);
            for (int k = 0; k < n; k++) fprintf(evlog, "%02x", s[k]);
            if (n == 0) fputc('-', evlog);
        }
    }
    fputc('\n', evlog);
}
void ev(const char *name, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vev(name, fmt, ap);
    va_end(ap);
}
void roger_log(const char *name, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vev(name, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- --script
 * --script=ms:action[:args],...  in time order (ms of real time when
 * interactive; of emulated time, 2000 instructions a millisecond, in
 * headless runs, where shot saves screenshot-N.bmp all the same). Actions: click:x:y (OS
 * units), tap:px:py (a host click at screen pixel px,py, through the real
 * SDL event path), poke:addr:value (write a memory word), release,
 * buttons:mask, move:dx:dy, esc, key:inkey[:ms] (INKEY -inkey held ms,
 * default 200), pad:control[:ms] and pad2:control[:ms] (a game controller's
 * control - up, a, start, ... (pad.c) - held ms, default 200),
 * type:ascii (into the keyboard buffer: OS_ReadC, INKEY), shot, shader[:dir]
 * (as F10: next, or -1 previous), quit */
#define MAXSCRIPT 8192
static struct { uint64_t ms; char act[8]; int a, b; } script[MAXSCRIPT];
static int nscript, script_i;
static uint64_t key_release_ms[129];
static uint64_t pad_release_ms[2][NPAD_CONTROLS];

static void parse_script(const char *p)
{
    while (*p && nscript < MAXSCRIPT) {
        int n = 0;
        script[nscript].ms = strtoull(p, (char **)&p, 10);
        if (*p == ':') p++;
        while (*p && *p != ':' && *p != ',' && n < 7) script[nscript].act[n++] = *p++;
        script[nscript].act[n] = 0;
        if (*p == ':' && !strncmp(script[nscript].act, "pad", 3)) {   /* a control, by name */
            int l = (int)strcspn(++p, ":,");
            if ((script[nscript].a = pad_control(p, l)) < 0) { fprintf(stderr, "--script: no control %.*s\n", l, p); exit(1); }
            p += l;
        } else if (*p == ':') { p++; script[nscript].a = strtol(p, (char **)&p, 0); }
        if (*p == ':') { p++; script[nscript].b = strtol(p, (char **)&p, 0); }
        if (*p == ',') p++;
        nscript++;
    }
}
int script_active(void) { return nscript > 0; }
int script_typing(void)                          /* keys still to be typed (type:) */
{
    for (int k = script_i; k < nscript; k++) if (!strcmp(script[k].act, "type")) return 1;
    return 0;
}

void script_run(void)
{
    /* headless: emulated time (deterministic) */
    uint64_t ms = rg_interactive ? (display_ticks_ns() - rg_t0_ns) / 1000000 : icount * 10 / TICK_STEPS;
    for (int k = 0; k < 129; k++)
        if (key_release_ms[k] && ms >= key_release_ms[k]) { key_down[k - 1] = 0; key_release_ms[k] = 0; }
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < NPAD_CONTROLS; c++)
            if (pad_release_ms[p][c] && ms >= pad_release_ms[p][c]) { pad_set(p, c, PAD_SCRIPT, 0); pad_release_ms[p][c] = 0; }
    while (script_i < nscript && ms >= script[script_i].ms) {
        const char *a = script[script_i].act;
        int x = script[script_i].a, y = script[script_i].b;
        if (!strcmp(a, "click")) { mouse_x = x; mouse_y = y; mouse_buttons = 4; }
        else if (!strcmp(a, "tap")) display_inject_tap(x, y);
        else if (!strcmp(a, "poke")) WR32((uint32_t)x, (uint32_t)y);
        else if (!strcmp(a, "release")) mouse_buttons = 0;
        else if (!strcmp(a, "buttons")) mouse_buttons = x;
        else if (!strcmp(a, "move")) { mouse_x += x; mouse_y += y; }
        else if (!strcmp(a, "esc")) escape_pressed = 1;
        else if (!strcmp(a, "key") && x >= 1 && x <= 128) {
            if (x == 65) kbd_status ^= 0x10;                /* Caps Lock (INKEY -65) toggles */
            key_down[x - 1] = 1; key_release_ms[x] = ms + (y > 0 ? y : 200); }
        else if (!strcmp(a, "pad") || !strcmp(a, "pad2")) {
            int p = a[3] == '2';
            pad_set(p, x, PAD_SCRIPT, 1); pad_release_ms[p][x] = ms + (y > 0 ? y : 200); }
        else if (!strcmp(a, "type")) key_buffer_put(x);
        else if (!strcmp(a, "shot")) screenshot_requested = 1;
        else if (!strcmp(a, "shader")) display_cycle_shader(x ? x : 1);   /* as F10 */
        else if (!strcmp(a, "quit")) quit_requested = 1;
        script_i++;
    }
}

/* ---------------------------------------------------------------- finish */
void finish(const char *why)
{
    char fn[512];
    static int exiting;
    if (rg->exited && !exiting && !strcmp(why, "returned to OS")) {
        exiting = 1;                             /* what !Run does after the program */
        rg->exited();
    }
    if (rg->finish) rg->finish();
    ramfs_remove();
    resourcefs_remove();
    fclose(evlog);
    snprintf(fn, sizeof fn, "%s.vdu", outprefix);
    FILE *f = fopen(fn, "wb"); video_save_vdu(f); fclose(f);
    snprintf(fn, sizeof fn, "%s.screen", outprefix);
    uint32_t dump = screen_size() + 0x20000 > 0x60000 ? screen_size() + 0x20000 : 0x60000;   /* all of it */
    if (screen_base() - 0x20000 + dump > MEMSIZE) dump = MEMSIZE - (screen_base() - 0x20000);
    f = fopen(fn, "wb"); fwrite(MEM + screen_base() - 0x20000, 1, dump, f); fclose(f);
    snprintf(fn, sizeof fn, "%s.regs", outprefix);
    f = fopen(fn, "w");
    for (int k = 0; k < 15; k++) fprintf(f, "r%d %08x\n", k, R[k]);
    fprintf(f, "psr %08x\n", PSR());
    fclose(f);
    cpu_report_interp();
    cpu_dump_pchist();
    audio_close();
    if (vdu_unhandled) fprintf(stderr, "VDU: %u unhandled codes/characters\n", vdu_unhandled);
    fprintf(stderr, "stopped: %s after %llu instructions (%llu interpreted, %llu SMC stores noted)\n",
            why, (unsigned long long)icount, (unsigned long long)interp_count,
            (unsigned long long)smc_count);
    exit(0);
}
void roger_finish(const char *why) { finish(why); }

/* ---------------------------------------------------------------- freezes
 * The window only stays alive while the main loop keeps reaching
 * rt_service(); anything that stops it (a runaway loop inside a handler
 * call, where interrupts are held off) freezes the window. The watchdog
 * reports where the CPU is once the main loop has gone 3s without
 * servicing the display; Ctrl-C / SIGTERM print the same plus the PC
 * histogram before exiting. */
static void on_signal(int sig)
{
    static volatile sig_atomic_t once;
    (void)sig;
    if (once++) _exit(1);
    cpu_report_state("interrupted");
    cpu_dump_pchist();
    _exit(1);
}
static void *watchdog(void *arg)
{
    (void)arg;
    int reported = 0;
    for (;;) {
        sleep(1);
        uint64_t last = rg_last_service_ns, now = display_ticks_ns();
        if (last && now - last > 3000000000ull) {
            if (!reported) { cpu_report_state("main loop stalled for 3s"); reported = 1; }
        } else reported = 0;
    }
    return NULL;
}

/* ---------------------------------------------------------------- start */
/* A loader-stage program (e.g. a title picture) run with the interpreter
 * before the program: it overlaps the program's addresses, so nothing
 * translated is used. */
int in_loader;                            /* running a loader program */

static void run_loader_program(const char *name)
{
    char path[2048];
    if (!hostpath(name, path)) return;
    long n = load_file(path, LOAD);
    if (n <= 0) return;
    uint32_t saved[16];
    memcpy(saved, R, sizeof R);
    R[13] = rg->stack ? rg->stack : ram_limit(); R[14] = RETURN_TO_OS; R[12] = 0;
    npc = LOAD;
    interp_only = in_loader = 1;
    run_until(RETURN_TO_OS);
    interp_only = in_loader = 0;
    memset(MEM + LOAD, 0, n);
    memcpy(R, saved, sizeof R);
    NF = ZF = CF = VF = 0;
}

static void usage(const char *prog)
{
    printf("usage: %s [options] [image]\n"
           "  --limit=N         headless: run N instructions, deterministic timing\n"
           "  --headless        headless with the default limit\n"
           "  --script=...      scripted input (see src/main.c)\n"
           "  --mips=N          the CPU's speed, interactive (0: unthrottled; default: the game's)\n"
           "  --mouse-sens=N    a captured mouse: OS units per host pixel (default: the game's, or 2)\n"
           "  --trace-gaps      interpret untranslated code, logging it to untranslated.log\n"
           "  --full-os         VDU and sprite drivers in headless runs too\n"
           "  --desktop[=WxH]   a Wimp program's desktop, drawn and used for real (WxH:\n"
           "                    its size in pixels, the window's to start with; 1024x768)\n"
           "  --windowed-games  ...the program's own screen in the window, not fullscreen\n"
           "  --no-title        skip loader programs\n"
           "  --scale=N --fullscreen[=WxH@Hz] --list-modes\n"
           "  --shader=NAME     display shader (F10 / Shift+F10 cycle; --crt = --shader=crt)\n"
           "  --list-shaders\n"
           "  --no-sound --wav=FILE\n"
           "  --pad=MAP --pad2=MAP  game controllers: the keys their controls hold, e.g.\n"
           "                    --pad=a=Z,b=X,start=Space (controls: up down left right a b x y\n"
           "                    lb rb lt rt back start guide ls rs; keys as SDL names them);\n"
           "                    --pad=off: none\n"
           "  --joystick        the controllers are the joystick the game reads, even headless\n"
           "                    (default: if the game reads one and a controller is connected);\n"
           "                    --joystick=off: they're keys only\n"
           "  --app=DIR         the program's directory   --save=DIR  where written files go\n"
           "  --floppy=FILE     the disc in drive 0 for sector reads (.hfe or .adf)\n"
           "  --out=PREFIX      output files PREFIX.events/.vdu/.screen/.regs\n%s",
           prog, rg->usage ? rg->usage : "");
}

int roger_main(int argc, char **argv, const struct roger_config *cfg)
{
    rg = cfg;
    const char *image = NULL;
    int scale = 4, headless = 0, no_sound = 0, no_title = 0;
    const char *pad_opt = NULL, *pad2_opt = NULL;
    const char *fs = NULL, *wavpath = NULL, *shader = NULL;
    float mips_opt = -1, sens_opt = -1;
    rg_appdir = cfg->appdir ? cfg->appdir : ".";
    rg_floppy = cfg->floppy;
    for (int k = 1; k < argc; k++)
        if (!strncmp(argv[k], "--limit=", 8) || !strcmp(argv[k], "--headless")) headless = 1;
    rg_interactive = rg_full_os = !headless;
    if (rg_interactive) rg_limit = ~0ull;
    for (int k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "--limit=", 8)) rg_limit = (uint64_t)strtod(argv[k] + 8, 0);
        else if (!strcmp(argv[k], "--headless")) ;
        else if (!strcmp(argv[k], "--full-os")) rg_full_os = 1;
        else if (!strcmp(argv[k], "--desktop")) rg_desktop = rg_full_os = 1;
        else if (!strncmp(argv[k], "--desktop=", 10)) {
            rg_desktop = rg_full_os = 1;
            if (sscanf(argv[k] + 10, "%dx%d", &rg_desktop_w, &rg_desktop_h) != 2) { usage(argv[0]); exit(1); }
        }
        else if (!strcmp(argv[k], "--windowed-games")) rg_windowed_games = 1;
        else if (!strncmp(argv[k], "--mips=", 7)) mips_opt = (float)atof(argv[k] + 7);
        else if (!strncmp(argv[k], "--mouse-sens=", 13)) sens_opt = (float)atof(argv[k] + 13);
        else if (!strcmp(argv[k], "--trace-gaps")) rg_trace_gaps = 1;
        else if (!strcmp(argv[k], "--no-title")) no_title = 1;
        else if (!strcmp(argv[k], "--fullscreen")) fs = "";
        else if (!strcmp(argv[k], "--no-sound")) no_sound = 1;
        else if (!strcmp(argv[k], "--pad=off")) rg_pads = 0;
        else if (!strcmp(argv[k], "--joystick")) rg_joystick_opt = 1;
        else if (!strcmp(argv[k], "--joystick=off")) rg_joystick_opt = 0;
        else if (!strncmp(argv[k], "--pad=", 6)) pad_opt = argv[k] + 6;
        else if (!strncmp(argv[k], "--pad2=", 7)) pad2_opt = argv[k] + 7;
        else if (!strcmp(argv[k], "--crt")) shader = "crt";
        else if (!strncmp(argv[k], "--shader=", 9)) shader = argv[k] + 9;
        else if (!strcmp(argv[k], "--list-shaders")) { shader_list(); exit(0); }
        else if (!strncmp(argv[k], "--wav=", 6)) wavpath = argv[k] + 6;
        else if (!strncmp(argv[k], "--fullscreen=", 13)) fs = argv[k] + 13;
        else if (!strcmp(argv[k], "--list-modes")) { display_list_modes(); exit(0); }
        else if (!strncmp(argv[k], "--scale=", 8)) scale = atoi(argv[k] + 8);
        else if (!strncmp(argv[k], "--script=", 9)) parse_script(argv[k] + 9);
        else if (!strncmp(argv[k], "--app=", 6)) rg_appdir = argv[k] + 6;
        else if (!strncmp(argv[k], "--out=", 6)) outprefix = argv[k] + 6;
        else if (!strncmp(argv[k], "--save=", 7)) rg_savedir = argv[k] + 7;
        else if (!strncmp(argv[k], "--floppy=", 9)) rg_floppy = argv[k] + 9;
        else if (!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h")) { usage(argv[0]); exit(0); }
        else if (cfg->arg && cfg->arg(argv[k])) ;
        else if (argv[k][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[k]); usage(argv[0]); exit(1); }
        else image = argv[k];
    }
    if (!pad_setup(pad_opt, pad2_opt)) exit(1);
    if (!rg_interactive) joystick_start(0);      /* (headless: only with --joystick) */
    rg_mips = mips_opt >= 0 ? mips_opt : cfg->mips;
    rg_mouse_sens = sens_opt > 0 ? sens_opt : cfg->mouse_sens > 0 ? cfg->mouse_sens : 2.0f;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (rg_interactive) { pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL); pthread_detach(wd); }

    MEM = calloc(1, MEMSIZE);
    char fn[512];
    snprintf(fn, sizeof fn, "%s.events", outprefix);
    evlog = fopen(fn, "w");
    if (!evlog) { perror(fn); exit(1); }
    if (rg_interactive) {
        if (!display_init(scale, fs, shader)) exit(1);
        if (!no_sound && !wavpath) audio_init_device();
        rg_t0_ns = display_ticks_ns();
        irq_start_clock(rg_t0_ns);
    }
    if (rg_desktop) desktop_init();
    if (wavpath && !audio_init_wav(wavpath)) fprintf(stderr, "cannot write %s\n", wavpath);
    if (!video_set_mode(cfg->initial_mode ? cfg->initial_mode : 12)) video_set_mode(12);   /* what !Run selected */
    if (cfg->desktop_pointer) pointer_shape = 1;       /* the Wimp's, left on */
    cli_init();
    if (cfg->directory && !set_csd(cfg->directory)) fprintf(stderr, "warning: no directory %s\n", cfg->directory);
    if (rg_full_os && !no_title && cfg->loader_programs) {
        for (int k = 0; cfg->loader_programs[k]; k++) run_loader_program(cfg->loader_programs[k]);
        recompute_irq();
    }

    /* the program (the arg hook may have changed cfg->image and image_path) */
    if (!image) image = cfg->image_path;
    roger_set_image(cfg->image);
    FILE *f = fopen(image, "rb");
    if (!f) { perror(image); exit(1); }
    long n = fread(MEM + cfg->image->base, 1, MEMSIZE - cfg->image->base, f);
    fclose(f);
    if (cfg->image->base + n != cfg->image->end) fprintf(stderr, "warning: image size differs from compiled image\n");
    bank_r13[3] = SVC_STACK; bank_r13[2] = IRQ_STACK;   /* privileged-mode stacks */
    disc_init();
    modules_init();
    modules_load();
    if (cfg->init) cfg->init();
    R[13] = cfg->stack ? cfg->stack : ram_limit(); R[14] = RETURN_TO_OS; R[12] = 0;
    npc = cfg->entry ? cfg->entry : cfg->image->base;
    recompute_irq();
    run_until(RETURN_TO_OS);
    finish("returned to OS");
}
