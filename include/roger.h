/* libroger - RISC OS Game Execution Runtime.
 *
 * Runs statically recompiled 26-bit ARM RISC OS programs (games, typically)
 * on a host: the emulated machine (CPU state, MEMC, VIDC, IOC timers), the
 * RISC OS calls such programs make (SWIs, vectors, events, the VDU and
 * sprite drivers, files, the sound system, relocatable modules) and an SDL3
 * front end (window, keyboard, mouse, audio).
 *
 * A game supplies the translated code (struct roger_image, from the code
 * generator) and a struct roger_config describing everything about it the
 * runtime can't know - where its files are, which mode !Run selects, what
 * memory it was given - then calls roger_main().
 */
#ifndef ROGER_H
#define ROGER_H
#include <stddef.h>
#include <stdint.h>
#include "roger_cpu.h"

/* A program file that may be loaded at its image's base while running
 * (e.g. a loader that chain-loads the main program to &8000). When OS_File
 * or *Load loads a file whose leaf name is `leaf` to `image->base`, the
 * image becomes the one dispatched once control next arrives at that
 * address. An overlay is code the program loads and calls alongside its
 * own (e.g. a BASIC program's CALL into a file it *Loads into a DIMmed
 * block): once loaded, its image is dispatched from as well, like a
 * module's. */
struct roger_program {
    const char *leaf;
    const struct roger_image *image;
    int overlay;
};

/* A relocatable module RMLoaded before the program starts (as !Run would):
 * the file is loaded at image->base (the module was translated for that
 * address), its initialisation entry is run, its SWI chunk is routed to its
 * SWI handler and its code is dispatched from image. An on_demand module is
 * only loaded when the program asks for that file (*RMLoad, *RMRun,
 * OS_Module 0/1), and may be killed again (*RMKill, OS_Module 4). Modules
 * that are never loaded at the same time may share an address, and so an
 * image when their code is the same (e.g. a music player module per tune). */
struct roger_module {
    const char *path;                     /* host path, relative to the working directory */
    const struct roger_image *image;
    int on_demand;
};

/* An image that copies itself and runs from the copy: while `image` is the
 * program image, an address in [to, to + len) is dispatched to the
 * translation at from + (a - to), with pc_bias = to - from so that the
 * code's view of its own PC stays right (translate it with --pcbias). */
struct roger_relocation {
    const struct roger_image *image;
    uint32_t from, to, len;
};

/* Deterministic headless input (--limit=N runs), at instruction counts. */
struct roger_click { uint64_t icount; uint32_t x, y, buttons; };   /* mouse event (OS units) */
struct roger_key { int inkey; uint64_t from, to; };                /* key held in [from, to) */

struct roger_config {
    const char *title;                    /* window title */

    /* --- the program */
    const char *image_path;               /* loaded at its image's base (normally &8000) and run;
                                             command-line overridable */
    uint32_t entry;                       /* where it starts, if not at its base (an absolute
                                             file's execution address) */
    const struct roger_image *image;      /* its translation */
    const struct roger_program *programs; /* other program files that may be loaded at their
                                             image's base later; NULL-terminated (leaf NULL) */
    const struct roger_relocation *relocations;   /* ends with image NULL */
    const struct roger_module *modules;   /* RMLoaded in order before the program; ends with path NULL */
    const char *const *loader_programs;   /* run (interpreted) before the program, as !Run
                                             might (e.g. a title picture); NULL-terminated.
                                             Only in interactive runs, unless --no-title */
    const char *command_line;             /* OS_GetEnv's command line (default "!RunImage") */

    /* --- files: the program's own directory on the host, and the RISC OS
     * prefixes meaning it (e.g. "<Game$Dir>.", "<Obey$Dir>."); "Disc1:" and
     * "Disc2:" (disc names) are always ignored */
    const char *appdir;
    const char *const *dir_prefixes;      /* NULL-terminated */
    const char *const *variables;         /* system variables set before the program runs (as
                                             !Run's *Set would): name, value, ..., NULL. A
                                             <name> in a filename or command line expands to its
                                             value, so e.g. {"Game$Dir", "!Game"} with appdir
                                             ".." finds <Game$Dir>.Data as ../!Game/Data */
    const char *directory;                /* the current directory as !Run left it (*Dir), e.g.
                                             "<Game$Dir>"; relative names are looked for there
                                             first. Default: the top of appdir */

    /* --- the machine as !Run left it */
    int initial_mode;                     /* screen mode */
    uint32_t wimpslot;                    /* application memory: the RAM limit is &8000 + this
                                             (until Wimp_SlotSize changes it) */
    uint32_t screen_size;                 /* total screen memory (default &14000; the program
                                             may change it: OS_ChangeDynamicArea) */
    uint32_t screen_base;                 /* where screen memory starts (default: so that it
                                             ends at &2000000, as RISC OS maps it) */
    uint32_t stack;                       /* initial R13 (default: the RAM limit) */
    const char *floppy;                   /* the disc in drive 0, for programs that read it
                                             sector by sector (FileCore_DiscOp, ADFS_DiscOp:
                                             copy protection): an HxC .hfe image (sector sizes
                                             and CRC errors as on the disc) or a flat .adf
                                             (800K or 1600K). --floppy=FILE overrides */
    uint32_t ramfs_size;                  /* a RAM disc of this many bytes (RAM:, empty at
                                             start, as !Run would have sized it); 0: none */
    float mips;                           /* interactive runs: the CPU's speed, millions of
                                             instructions a second (0: as fast as the host
                                             runs it). For games that don't wait for the
                                             frame and so run at the speed of the machine:
                                             an 8MHz ARM2 is about 4.5. --mips=N overrides */
    int monitor_type;                     /* the configured monitor type modules are asked about
                                             (Service_ModeExtension R3): 0 a TV-standard monitor
                                             (the default), 1 multisync, 3 VGA - a module that
                                             only defines its modes for one (e.g. 480-line VGA
                                             modes) needs it */
    int vidc20;                           /* a Risc PC rather than an Archimedes: VIDC20's
                                             modes (taller ones, multisync numbered modes,
                                             RISC OS 3.5 mode selectors, 16 and 32 bits per
                                             pixel, gamma tables via PaletteV), and what
                                             programs check for one - Wimp 3.50,
                                             OS_ScreenMode, OS_Memory 8 (2MB of VRAM),
                                             their names for OS_SWINumberFromString */

    /* --- interrupts */
    int irq1v;                            /* also call a handler whose address the program
                                             stores at &100 (a low-level IRQ dispatch slot
                                             some games use directly) for each interrupt */
    int irq1v_timer_always;               /* ...and call it for timer 1 every frame, even
                                             when the timer isn't running */

    /* --- input */
    int pointer_y_offset;                 /* OS units between the host pointer and the game's
                                             pointer hit point */
    float mouse_sens;                     /* ...then OS units per host pixel moved (0: 2); a
                                             game that steers by how far the pointer moved
                                             wants less, as an Archimedes mouse gave far fewer
                                             counts than a modern one. --mouse-sens=N overrides */
    int relative_mouse;                   /* capture the host mouse (relative motion) while the
                                             program keeps re-centring the pointer */
    int ctrl_meta_keys;                   /* the runtime's keys are Ctrl+F10/F11/F12 (shaders,
                                             fullscreen, screenshot), leaving F10-F12 to the game */
    const struct roger_click *clicks;     /* headless scenario */
    unsigned nclicks;
    const struct roger_key *keys;
    unsigned nkeys;
    const char *pad_map;                  /* game controllers: the keys their controls hold,
                                             "control=key,..." over libroger's default (src/pad.c:
                                             up down left right a b x y lb rb lt rt back start
                                             guide ls rs; keys by SDL's names, e.g. "Z", "Return",
                                             "'"); --pad= overrides */
    const char *pad2_map;                 /* a second controller's (default: the first's) */
    int joystick;                         /* joystick interfaces the program reads, which
                                             libroger answers from the controllers: ROGER_JOY_*
                                             (src/pad.c). On when a controller is connected at
                                             start, or --joystick; the D-pad, left stick, a and b
                                             are then the joystick's, not keys */

    /* --- translation gaps: [from, to) address pairs, ending 0,0, that are
     * interpreted silently. Any other untranslated address stops the run
     * (or, with --trace-gaps, is logged and interpreted). With
     * interpret_gaps set, every gap is interpreted silently. */
    const uint32_t *interpreted;
    int interpret_gaps;

    /* --- hooks (all optional) */
    void (*init)(void);                   /* program loaded, before it runs */
    void (*frame)(void);                  /* every vsync, after the vsync interrupt handlers */
    void (*centisecond)(void);            /* every centisecond tick */
    int  (*swi)(uint32_t swi, int x);     /* first look at every SWI: 1 if handled (x: X bit) */
    int  (*call_addr)(uint32_t a);        /* first look at every dispatch: 1 if handled */
    int  (*arg)(const char *arg);         /* a command-line argument libroger doesn't know: 1 if used */
    void (*exited)(void);                 /* the program returned to the OS (what !Run does
                                             next, e.g. a closing screen); the run then ends */
    void (*finish)(void);                 /* the run is ending */
    const char *usage;                    /* extra --help text for arg() */
    void (*wimp_idle)(void);              /* Wimp_Poll has no event to return: queue some
                                             (roger_wimp_click() &c.), standing in for the
                                             user at the desktop */
    void (*filer_run)(const char *name);  /* on the desktop, a directory viewer's application,
                                             or a file the program didn't load, double-clicked:
                                             what *Run would do with it (there's only the one
                                             program). Without it, reported on stderr */
};

/* roger_config.joystick: the interfaces, and how directions are given */
#define ROGER_JOY_ACORN    1   /* Acorn's Joystick module: Joystick_Read (&43F40) */
#define ROGER_JOY_KM       2   /* Keith Martin's (The Fourth Dimension's): Joy_Read0/1 (&CFFC0) */
#define ROGER_JOY_ANALOGUE 4   /* Joystick_Read gives the left stick's position (-127..127),
                                  not a digital joystick's -64, 0 or 64 */

/* Parse the command line, set up the machine and run the program; does not
 * return (exits when the program ends, the window is closed, or the
 * instruction limit is reached). */
int roger_main(int argc, char **argv, const struct roger_config *cfg) __attribute__((noreturn));

/* --- services for game code */
#define ROGER_LOAD        0x8000u         /* application load address */
#define ROGER_HANDLER_RET 0x3FFFFE0u      /* return address meaning "back to the runtime" */
#define ROGER_MODE_USR    0u
#define ROGER_MODE_SVC    3u
#define ROGER_MODE_IRQ    (2u | 0x08000000u)   /* IRQ mode, IRQs disabled */

void roger_log(const char *name, const char *fmt, ...);   /* event log: 'i' word, 's' bytes+len */
void roger_finish(const char *why) __attribute__((noreturn));
void roger_call(uint32_t addr, uint32_t mode, uint32_t r14);   /* run code until HANDLER_RET */
void roger_call_program(uint32_t addr, uint32_t r14);          /* ...as the program (USR, interrupts taken) */
void roger_run_until(uint32_t stop);
void roger_idle(unsigned n);                           /* host code waiting: n instructions' worth of time passes */
void roger_set_image(const struct roger_image *img);  /* dispatch addresses through img */
const struct roger_image *roger_current_image(void);
long roger_load_file(const char *path, uint32_t addr);
int  roger_hostpath(const char *name, char *out);     /* RISC OS filename -> host path (2048 bytes) */
int  roger_interactive(void);                          /* 0 in headless (--limit) runs */
int  roger_in_handler(void);                           /* inside an interrupt handler call */
void roger_log_sound_swi(uint32_t swi);                /* the event line the sound SWIs log */
void roger_swi(uint32_t swi);                          /* call a SWI from game code with R[] as set;
                                                          VF reports an error */
void roger_cli(const char *command);                   /* OS_CLI from game code */
uint32_t roger_scratch(const char *s);                 /* copy a string into emulated memory (a
                                                          small runtime buffer) for a SWI argument */
const char *roger_getvar(const char *name);            /* a system variable (NULL if unset) */
void roger_setvar(const char *name, const char *value);
/* The desktop (wimp.c): events for Wimp_Poll to return, in order */
void roger_wimp_event(uint32_t reason, const uint32_t *block, int nwords);
void roger_wimp_click(int32_t window, int32_t icon, uint32_t buttons);   /* Mouse_Click; window -2
                                                          is the icon bar; buttons 4 select, 2 menu */
void roger_wimp_menu(const int *items);                /* Menu_Selection: item indices, ending -1 */
void roger_wimp_quit(void);                            /* Message_Quit */
int32_t roger_wimp_menu_window(void);                  /* the dialogue box open as a menu, or 0 */
void roger_wimp_save_to(const char *path);             /* the next save dragged out goes to path
                                                          (a Filer under the pointer; "" none) */
int  roger_filer_open(const char *dir);                /* on the desktop, a directory viewer
                                                          (*Filer_OpenDir): 0 if not (no desktop) */
int  roger_filer_load(const char *name, int32_t window, int32_t icon);   /* the file or application
                                                          given to the program (Message_DataLoad)
                                                          as if dragged from a viewer to the window
                                                          and icon (-2: the icon bar): 0 if none */
uint32_t roger_wimp_icon_buffer(int32_t window, int icon);   /* an icon's indirected text (0: none) */
void roger_audio_out(const int16_t *lr, int frames, int rate);   /* host audio (stereo) */
extern uint32_t vidc_palette[16];                      /* live VIDC palette (0x0BGR) */
extern uint32_t vidc_palette_top[16];                  /* palette above palette_split */
extern int palette_split;                              /* display line where vidc_palette starts */

#endif
