/* libroger internals shared between the source files. */
#ifndef ROGER_INTERNAL_H
#define ROGER_INTERNAL_H
#include <stdio.h>
#include <stdint.h>
#include "roger.h"

/* ---- the emulated machine's memory map (see also SYS_SPRITES)
 *   &0000000  page zero (hardware vectors; &100 see irq1v)
 *   &0008000  the application (to the RAM limit, &8000 + WimpSlot)
 *   &17F0000  runtime workspace: sound DMA buffers, sound channel control
 *             blocks and tables, the mode selector, OS_GetEnv's strings,
 *             SVC and IRQ stacks, a scratch buffer, the error block, the
 *             built-in voice, module private words (&17FFFFC down)
 *   &1800000  RMA: RMLoaded modules, then OS_Module claims (&1810000 on)
 *   ........  screen memory, ending at &2000000 (configurable)
 *   &3200000  IOC, &3400000 VIDC, &3600000 MEMC (writes are emulated) */
#define MEMSIZE      0x4000000u
#define SYS_SPRITES  0x3000000u           /* the system sprite area (*SLoad, OS_SpriteOp < 256) */
#define SYS_SPRITES_MAX 0x100000u
#define WIMP_SPRITES 0x3100000u           /* the Wimp's sprite pool (*IconSprites, Wimp_SpriteOp) */
#define FILER_DATA   0x2F00000u           /* the Filer's windows' indirected data (filer.c) */
#define RETURN_TO_OS 0x3FFFFF0u           /* R14 the program starts with */
#define HANDLER_RET  ROGER_HANDLER_RET
#define LOAD         ROGER_LOAD
#define DMA_BUF      0x017F0000u          /* two sound DMA buffers, &2000 each */
#define SCCB_AREA    0x017F4000u          /* 8 sound channel control blocks, &100 each */
#define MODE_SEL     0x017F6C00u          /* the current mode's selector (OS_ScreenMode 1) */
#define ENV_AREA     0x017F7000u          /* OS_GetEnv's command line and start time */
#define SVC_STACK    0x017F8000u          /* R13_svc / R13_irq stack tops */
#define IRQ_STACK    0x017FC000u
#define SCRATCH      0x017FC000u          /* 1K for strings game code passes to SWIs (above the
                                             IRQ stack: the sound tables run to &17F6A00) */
#define ERROR_BUF    0x017FC400u          /* the error block a failing SWI returns (os_error) */
#define BEEP_VOICE   0x017FC500u          /* WaveSynth-Beep's voice header (sound.c) */
#define MOD_PRIVATE  0x017FFFFCu          /* module k's private word: MOD_PRIVATE - 4k */
#define MOD_POSTFIX  0x017FFE00u          /* "Base": every module's one incarnation (OS_Module 12) */
#define ROM_MODULES  0x03800000u          /* RISC OS modules' stand-ins (disc.c), where the ROM is */
#define ROM_MODULES_END 0x03803000u
#define RMA_BASE     0x01800000u
#define RMA_HEAP     0x01810000u
#define SCREEN_BASE_MIN 0x01E00000u       /* the RMA may grow to here (screen memory is above) */
#define IOC          0x3200000u
#define MODE_SVC     ROGER_MODE_SVC
#define MODE_IRQ     ROGER_MODE_IRQ

/* headless runs advance emulated time by instruction count */
extern uint64_t tick_steps;              /* headless runs: instructions per centisecond (20000,
                                            2 MIPS, unless roger_config.headless_mips) */
#define TICK_STEPS   tick_steps
#define VSYNC_STEPS  (2 * tick_steps)     /* instructions per frame (50Hz) */
#define CS_NS        10000000ull

extern const struct roger_config *rg;     /* the game's configuration */

/* main.c */
extern int rg_interactive, rg_full_os, rg_trace_gaps;
extern float rg_mouse_sens;               /* captured mouse: OS units per host pixel */
extern float rg_mips;                     /* interactive CPU speed (MIPS), 0 unthrottled */
extern uint64_t rg_limit, rg_t0_ns;
extern const char *rg_appdir, *rg_savedir, *rg_floppy;
void ev(const char *name, const char *fmt, ...);
void os_error(uint32_t num, const char *msg);   /* a SWI fails: R0 -> error block, V set */
void finish(const char *why) __attribute__((noreturn));
void script_run(void);
int  script_active(void);
int  script_typing(void);                    /* --script has type: actions to come */

/* cpu.c */
extern uint32_t bank_r13[4], bank_r14[4];
extern int in_handler, interp_only, in_loader;
extern uint32_t g_cur_pc;
extern uint64_t interp_count, smc_count;
void call_code(uint32_t addr, uint32_t mode, uint32_t r14);
void run_callbacks(void);                 /* transient callbacks due, if in USR mode with IRQs on */
void call_swi_code(uint32_t addr, uint32_t mode, uint32_t r14);   /* interruptible (a SWI handler) */
void run_until(uint32_t stop);
void interp_step(void);
void cpu_add_image(const struct roger_image *img);    /* dispatched wherever it lies (modules) */
void cpu_remove_image(const struct roger_image *img);
int  cpu_translated(uint32_t a);
void cpu_report_state(const char *why);
void cpu_dump_pchist(void);
void cpu_report_interp(void);
int  cpu_seen_gap(uint32_t a);
void cpu_log_gap(uint32_t a);
void cpu_programs_loaded(const char *name, uint32_t addr);   /* OS_File loaded a file */

/* clib.c: the Shared C Library */
int  clib_swi(uint32_t b);                 /* SharedCLibrary_LibInit... */
int  clib_dispatch(uint32_t a);           /* an entry in a client's library vector */

/* sound.c */
int  sound_channel_voice(int chan, const char *voice);   /* *ChannelVoice */

/* irq.c */
extern uint32_t monotonic, events_enabled;
void recompute_irq(void);
void rt_service(int vsync_now);
void wait_vsync(void);
void call_vector(unsigned vec, int nregs, const uint32_t *regs);
int  irq_swi(uint32_t b);                 /* OS_Claim/Release, device vectors */
int  irq_sound_claimed(void);             /* a program has the sound DMA interrupt (device 9) */
void irq_sound(void);                     /* ...call its handler */

/* video.c */
struct rg_mode { int width, height, bpp, xeig, yeig, text_cols, text_rows;
                 float aspect; };                /* display width/height; 0: 4:3, the monitor */
extern uint32_t screen_mode;             /* a number, or a selector's address */
extern uint32_t bank, display_bank;
const struct rg_mode *mode_info(uint32_t m);   /* NULL if not a mode this machine has */
int  video_set_mode(uint32_t m);          /* 0 if not a mode */
const struct rg_mode *cur_mode(void);
float mode_aspect(const struct rg_mode *m);   /* the shape it's shown at */
uint32_t screen_base(void);               /* start of screen memory */
uint32_t screen_size(void);               /* total screen memory */
uint32_t mode_bytes(const struct rg_mode *m);
uint32_t write_bank_addr(void);           /* screen memory the VDU drivers write to */
void vdu_put(uint8_t c);
const uint8_t *displayed(void);           /* the frame as the display hardware shows it */
uint32_t vduvar(uint32_t v);
uint32_t modevar(int64_t mode, uint32_t v);   /* mode -1: the current one */
int  video_os_word(uint32_t r0, uint8_t *blk);   /* OS_Word 12/21/22 */
int  video_timer_line(uint32_t latch);    /* output line where timer 1 (started at flyback) fires */
void video_set_bank(int which, uint32_t b);
void video_set_screen_size(uint32_t size);   /* OS_ChangeDynamicArea 2 */
uint32_t vidc_rgb(int line, uint8_t pixel);   /* host colour of a pixel on an output line */
void video_default_palette(void);         /* the palette a mode change sets */
uint32_t video_colour(uint8_t p);         /* the current mode's colour p now (0xRRGGBB) */
uint32_t video_default_colour(int bpp, uint8_t p);   /* a default palette's (0xRRGGBB) */
int  colourtrans_swi(uint32_t b);
int  buffers_swi(uint32_t b);             /* buffers.c: the Buffer Manager */
int  heap_swi(uint32_t b);                /* heap.c: OS_Heap */
int  dynarea_swi(uint32_t b);             /* dynarea.c: OS_DynamicArea (a Risc PC's) */
int  dynarea_read_change(uint32_t b);     /* ...OS_Read/ChangeDynamicArea on its areas */
int  buffers_os_byte(uint32_t op);        /* ...OS_Byte 138, 145, 152, 153, 21 on its buffers */
void video_save_vdu(FILE *f);
int  video_palettev(void);                /* PaletteV's default handler: 1 if handled */
extern uint8_t gamma_tab[3][256];         /* VIDC20 gamma tables (PaletteV 9), if gamma_set */
extern int gamma_set;             /* the VDU stream so far */
extern int mbox[4];                       /* OS_Word 21,1 mouse bounding box */
extern int pointer_shape;                 /* OS_Byte 106: the OS pointer shown (1-4), or 0 off */
struct pointer_def { int w, h, ax, ay; uint8_t pix[32 * 64]; };   /* pixels 0-3, 32 a row; */
extern struct pointer_def pointer_defs[16];      /* w 0: not defined (by shape number) */
extern uint32_t pointer_rgb[4];                  /* its colours 1-3 (0xRRGGBB) */
extern int pointer_gen;                          /* changes when a shape or colour does */

/* vdu.c: the VDU drivers (interactive runs) */
extern unsigned vdu_unhandled;
void vdu_write(uint8_t c);
void vdu_mode_reset(void);                /* after a mode change: windows, colours, CLS */
void vdu_char_def(int c, uint8_t *def);   /* OS_Word 10: a character's 8 rows */
int  sprite_op_full(void);
void vdu_paint_pixel(int x, int y, int v);   /* (fonts.c) pixel coordinates, y down; clipped */
void vdu_origin(int *x, int *y);          /* the graphics origin, OS units */
void vdu_text_cursor(int *x, int *y);     /* the text cursor, in the text window */
uint8_t vdu_colour_pixel(int c);          /* a GCOL colour number's pixel value */

/* fonts.c: the Font Manager (RISC OS 2's pixel fonts) */
int  fonts_swi(uint32_t b);
int  fonts_swi_number(const char *name, uint32_t *n);

/* sound.c */
void sound_dma_advance(uint32_t us);
uint32_t sound_level0_due_us(void);       /* a program driving the DMA: us until its buffer ends */
void sound_tick(void);                    /* every centisecond */
int  sound_swi(uint32_t b);
void sound_memc(int reg, uint32_t v);     /* MEMC sound DMA registers: 4 Sstart, 5 SendN, 6 Sptr */
void sound_vidc(unsigned reg, uint32_t val);   /* VIDC1's sound registers, written directly */
extern uint32_t memc_control;           /* MEMC's control register (bit 11: sound DMA on) */

/* audio.c: host audio */
int  audio_init_device(void);
int  audio_init_wav(const char *path);
void audio_close(void);

/* files.c */
extern int swi_x;                         /* the current SWI is X-flagged */
int  cstr(uint32_t a, char *out, int term_ctrl);
int  hostpath(const char *name, char *out);
int  hostpath_read(const char *name, char *out);   /* preferring a saved copy */
int  set_csd(const char *name);                   /* *Dir, OS_FSControl 0: 0 if no such directory */
void file_info(const char *path, uint32_t *load, uint32_t *exec);
int  save_file(const char *name, uint32_t addr, uint32_t len);
long load_file(const char *path, uint32_t addr);
uint32_t sprite_area(void);               /* the area OS_SpriteOp R0/R1 name */
const char *ramfs_dir(void);              /* the RAM disc's host directory (NULL: none) */
uint32_t ramfs_used(void);                /* bytes of files on it */
void ramfs_remove(void);                  /* at exit: the RAM disc goes */
int  copy_object(const char *from, const char *to, int recurse);   /* *Copy: 0 if it failed */
int  delete_object(const char *name);    /* *Delete: 0 if there was nothing */
uint32_t sys_sprites(void);               /* the system sprite area */
uint32_t wimp_sprites(void);              /* the Wimp's sprite pool */
int  vdu_schoose(const char *name);       /* *SChoose */
int  vdu_screenload(const uint8_t *f, size_t n);   /* *ScreenLoad: a sprite file's first sprite */
int  screen_load(const char *name);        /* ...from a file: 0 if it isn't there */
int  sprite_file(uint32_t area, const char *name, int merge);   /* *SLoad / *SMerge: 0 if not found */
int  sprite_file_host(uint32_t area, const char *path, int merge);   /* ...a host file */
int  files_swi(uint32_t b);

/* resourcefs.c: ResourceFS (Resources:$) */
int  resourcefs_swi(uint32_t b);
const char *resourcefs_name(const char *n, const char **hostdir);   /* a Resources: name: the rest */
int  resourcefs_file(const char *name, uint32_t *data, uint32_t *len);   /* a registered file in memory */
void resourcefs_remove(void);             /* at exit: its host directory goes */

/* messagetrans.c: MessageTrans */
int  messagetrans_swi(uint32_t b, int x);

/* disc.c */
void disc_init(void);
int  disc_swi(uint32_t b);
int  disc_rom_module(const char *name, uint32_t *base, uint32_t *word);

/* swi.c */
extern uint32_t env[17][3];               /* OS_ChangeEnvironment's handlers (1: undefined instruction) */
struct dir_entry {                        /* a directory entry (dir_entries) */
    char leaf[64];                        /* its RISC OS name ("/" for a host ".") */
    uint32_t load, exec, size;            /* catalogue information */
    int isdir;
};
int  dir_entries(const char *name, struct dir_entry *e, int max);   /* -> count, -1: no directory */
void sprite_op(void);

/* modules.c */
void modules_load(void);
int  modules_rmload(const char *name, const char *args);   /* (args NULL: the configuration's) 0: not a configured module file */
int  modules_rmkill(const char *title);
void modules_rmclear(void);                 /* *RMClear: every module killed */
int  modules_swi(uint32_t b, int x);
int  modules_swi_number(const char *name, uint32_t *n);   /* a loaded module's SWI by name */
int  modules_gap(uint32_t a);             /* an untranslated address in a module */
int  modules_command(const char *cmd, const char *tail, int x);   /* a module's *command: 1 if found */
int  modules_service(void);               /* service call R1 (R0-R9 as set): 1 if claimed */
void modules_service_call(uint32_t reason, uint32_t r2);   /* ...the OS's own, registers kept */
int  modules_version(const char *title);  /* a module's version in hundredths, -1: none */
void modules_init(void);                  /* the OS's own modules' headers (OS_Module 18) */
uint32_t rma_size(void);                  /* the RMA's extent so far */
void rom_module(uint32_t base, const char *title, const char *help, uint32_t chunk, const char *const *swis);
int  version_hundredths(const char *v);   /* "1.36" -> 136 */

/* cli.c: OS_CLI and system variables */
void cli_init(void);
void gstrans(const char *in, char *out, int size);
int  cli_swi(uint32_t b, int x);

/* swi.c */
extern uint32_t swi_pc;
int  pressed(int inkey);

/* wimp.c: the Window Manager, the application slot, the screen's dynamic area */
uint32_t ram_limit(void);                 /* &8000 + the application slot */
int  wimp_swi(uint32_t b);
int  wimp_version(void);                  /* what Wimp_Initialise reports, 316 for 3.16 */

/* Windows and icons as the program defines them (wimp.c keeps them,
 * desktop.c draws them): a window is its Wimp_CreateWindow block (88 bytes:
 * +0 visible area, +16 scroll offsets, +24 behind, +28 flags, +32 colours,
 * +40 work area extent, +56 title flags, +60 work area flags, +64 sprite
 * area, +68 minimum size, +72 title data, +84 icon count) and its icons (32
 * bytes each: +0 bounding box, +16 flags, +20 data). The icon bar is window
 * -2 (its icons on the left) and -1 (on the right). */
#define WIMP_MAXWIN  64
#define WIMP_MAXICON 128
#define WIN_OPEN     0x10000              /* window flags bit 16: open */
struct win {
    uint32_t handle;
    uint8_t blk[88];
    uint8_t icon[WIMP_MAXICON][32];
    uint8_t used[WIMP_MAXICON];
    int nicons;
    /* the desktop's (--desktop) */
    int redraw;                           /* a Redraw_Window_Request is due */
    uint8_t *back;                        /* what the program last drew in it: screen memory */
    uint32_t back_size;                   /* (valid within its visible area) */
    int32_t back_vis[6];                  /* the visible area and scroll it was drawn for */
    int32_t back_dx, back_dy;             /* ...moved by this to lie on the program's screen */
    int32_t full_prev[4];                 /* toggle size: the visible area before */
    int filer;                            /* the Filer's (a directory viewer), not the program's */
};
extern struct win wimp_iconbar;
extern int8_t wimp_bar_side[WIMP_MAXICON];   /* an icon bar icon's side: -2 left, -1 right */
extern uint32_t wimp_caret[6];            /* window, icon, x, y, height+flags, index */
extern uint32_t wimp_menu_win;            /* the dialogue box open as a menu, 0 if none */
struct win *wimp_window(uint32_t handle); /* NULL if none; the icon bar for -1 and -2 */
int  wimp_bar_icon(const uint8_t *icon, int side);   /* an icon bar icon of the runtime's own: -2 left */
int  wimp_stack(struct win **out);        /* the open windows, bottom to top */
void wimp_open(struct win *w, const uint8_t *open, uint32_t behind);   /* open: visible area, scroll */
void wimp_close(struct win *w);
void wimp_set_caret(const uint32_t c[6]); /* with Lose_Caret / Gain_Caret */
void wimp_auto_select(uint32_t window, int icon, uint32_t buttons);   /* a radio icon clicked */
uint32_t wimp_rd(const uint8_t *p);       /* a little-endian word of a block */
struct win *wimp_new_window(const uint8_t *blk);   /* Wimp_CreateWindow's block (no icons); NULL: too many */
void wimp_delete(struct win *w);
uint32_t wimp_new_ref(void);              /* a message's my_ref */

/* filer.c: the Filer - directory viewers on the desktop, and its end of
 * the data transfer protocol */
#define FILER_WINDOW 0x46494C45u          /* without the desktop: the viewer a save is dropped on */
#define FILER_TASK   0x52454C46u
#define FILER_BAR    0xFFFFFFFEu          /* filer_message: to its icon bar icon */
int  filer_open(const char *dir);         /* *Filer_OpenDir: 0 if not (no desktop, no directory) */
void filer_close(const char *dir);        /* *Filer_CloseDir */
int  filer_owns(uint32_t handle);         /* a viewer's window */
int  filer_bar_icon(void);                /* its icon bar icon, -1 */
void filer_event(uint32_t reason, const uint32_t *blk);   /* the user's, on a viewer */
void filer_drag_end(void);                /* a drag from a viewer ended (at the pointer) */
void filer_message(uint32_t m, uint32_t to);   /* Wimp_SendMessage to it (a window, or FILER_TASK) */
void filer_delivered(uint32_t reason, const uint32_t *blk);   /* Wimp_Poll returned an event */
void filer_polled(void);                  /* the program polls again */
int  filer_armed(void);                   /* roger_wimp_save_to() is waiting for a save */
void filer_refresh(void);                 /* reread the viewers' directories */
void filer_tick(void);                    /* ...now and then */
void filer_app_sprites(const char *hostdir);   /* an application's !Sprites into the Wimp's pool */

/* desktop.c: the desktop drawn and used for real (--desktop), in place of
 * the wimp_idle hook */
extern int rg_desktop, rg_desktop_w, rg_desktop_h, rg_windowed_games;
int  desktop_visible(void);
int  desktop_size(int *w, int *h);        /* in OS units */
void desktop_init(void);
void desktop_enter(void);                 /* the program is (back) in the desktop */
void desktop_mode_changed(void);          /* ...and has left it for a screen of its own */
void desktop_input(void);                 /* in Wimp_Poll: the user's actions, as events */
void desktop_buttons_changed(int x, int y, int b);   /* a vsync saw the mouse buttons change */
void desktop_wait(void);                  /* ...nothing for the program yet: time passes */
void desktop_present(void);
void desktop_screenshot(const char *path);
void desktop_create_menu(uint32_t menu, int x, int y);   /* Wimp_CreateMenu (-1 closes) */
void desktop_create_submenu(uint32_t menu, int x, int y);
void desktop_drag_box(uint32_t blk);      /* Wimp_DragBox (0: cancel) */
void desktop_drag_filer(const int32_t *box);   /* the Filer drags an icon: filer_drag_end() */
int  desktop_pointer(int *x, int *y, int32_t *window, int32_t *icon);   /* -> buttons */
void desktop_window_gone(struct win *w);  /* closed or deleted */
void desktop_outline(const struct win *w, int32_t *o);   /* its visible area and furniture */

/* host.c: the SDL front end */
int  display_init(int scale, const char *fullscreen, const char *shader);   /* fullscreen: NULL, "" or "WxH[@Hz]" */

/* shader.c: display shader chains on the GPU renderer's device */
struct SDL_Renderer;
struct SDL_Texture;
int  shader_init(struct SDL_Renderer *ren);   /* 0: unavailable */
int  shader_count(void);
const char *shader_name(int i);
int  shader_find(const char *name);           /* -1 if none */
int  shader_select(int i);                    /* -1: none; 0 if it failed */
struct SDL_Texture *shader_run(const uint32_t *rgba, int w, int h, int vw, int vh);
void shader_list(void);                       /* --list-shaders */
void display_toggle_fullscreen(void);
void display_cycle_shader(int dir);       /* F10 / Shift+F10 */
void display_list_modes(void);
void display_present(const uint8_t *pixels);
void display_present_rgb(const uint32_t *rgb, int w, int h, float aspect);   /* &RRGGBB pixels */
void display_present_desktop(const uint32_t *rgb, int w, int h);   /* ...pixel for pixel */
void display_output_size(int *w, int *h); /* the window's size in pixels */
int  display_fullscreen(void);
void display_set_fullscreen(int on);
void display_screenshot_rgb(const uint32_t *rgb, int w, int h, const char *path);
void display_poll(void);
uint64_t display_ticks_ns(void);
void display_sleep_ns(uint64_t ns);
void display_set_mouse(int x, int y);
void display_inject_tap(int px, int py);
void display_screenshot(const uint8_t *pix, const char *path);
extern uint8_t kbd_status;                /* OS_Byte 202: bit 4 clear Caps Lock on, bit 5
                                             clear Shift Lock on; 3 Shift, 6 Ctrl down (live) */
extern uint8_t key_down[128];             /* by RISC OS internal key number */
/* game controllers as the keyboard (pad.c) */
#define NPAD_CONTROLS 17
enum { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_LT, PAD_RT,
       PAD_BACK, PAD_START, PAD_GUIDE, PAD_LS, PAD_RS };
enum { PAD_BUTTON = 1, PAD_STICK = 2, PAD_SCRIPT = 4 };   /* what holds a control */
extern uint8_t pad_down[128];             /* controls holding each internal key */
extern int rg_pads;                       /* use the host's controllers */
int  key_held(int k);                     /* internal key k down: the keyboard or a controller */
int  pad_control(const char *name, int len);   /* a control's number, -1 if no such name */
int  pad_map(int pad, const char *mapping);
int  pad_setup(const char *opt1, const char *opt2);   /* the mappings: 0 if one is bad */
void pad_set(int pad, int control, int source, int down);
void pad_release_all(int pad);
int  host_internal_key(int scancode);     /* SDL scancode -> internal key number, -1 */
extern int rg_joystick;                   /* the joystick interfaces answered (0: none) */
extern int rg_joystick_opt;               /* --joystick: 1, =off: 0, else -1 */
void joystick_start(int controllers);
void pad_axis(int pad, int axis, int value);   /* left stick x (0) or y (1), SDL's -32768..32767 */
int  joystick_swi(uint32_t swi);          /* 1 if it's a joystick SWI, answered */
extern int mouse_x, mouse_y, mouse_buttons;   /* OS units; 4 select, 2 menu, 1 adjust */
extern int quit_requested, escape_pressed, screenshot_requested;
int  key_buffer_get(void);
void key_buffer_put(int c);
void key_buffer_flush(void);

#endif
