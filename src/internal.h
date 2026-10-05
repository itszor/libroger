/* libroger internals shared between the source files. */
#ifndef ROGER_INTERNAL_H
#define ROGER_INTERNAL_H
#include <stdio.h>
#include <stdint.h>
#include "roger.h"

/* ---- the emulated machine's memory map
 *   &0000000  page zero (hardware vectors; &100 see irq1v)
 *   &0008000  the application (to the RAM limit, &8000 + WimpSlot)
 *   &17F0000  runtime workspace: sound DMA buffers, sound channel control
 *             blocks, OS_GetEnv's strings, SVC and IRQ stacks, module
 *             private words (&17FFFFC down)
 *   &1800000  RMA: RMLoaded modules, then OS_Module claims (&1810000 on)
 *   ........  screen memory, ending at &2000000 (configurable)
 *   &3200000  IOC, &3400000 VIDC, &3600000 MEMC (writes are emulated) */
#define MEMSIZE      0x4000000u
#define RETURN_TO_OS 0x3FFFFF0u           /* R14 the program starts with */
#define HANDLER_RET  ROGER_HANDLER_RET
#define LOAD         ROGER_LOAD
#define DMA_BUF      0x017F0000u          /* two sound DMA buffers, &2000 each */
#define SCCB_AREA    0x017F4000u          /* 8 sound channel control blocks, &100 each */
#define ENV_AREA     0x017F7000u          /* OS_GetEnv's command line and start time */
#define SVC_STACK    0x017F8000u          /* R13_svc / R13_irq stack tops */
#define IRQ_STACK    0x017FC000u
#define MOD_PRIVATE  0x017FFFFCu          /* module k's private word: MOD_PRIVATE - 4k */
#define RMA_BASE     0x01800000u
#define RMA_HEAP     0x01810000u
#define IOC          0x3200000u
#define MODE_SVC     ROGER_MODE_SVC
#define MODE_IRQ     ROGER_MODE_IRQ

/* headless runs advance emulated time by instruction count */
#define TICK_STEPS   20000                /* instructions per centisecond */
#define VSYNC_STEPS  40000                /* instructions per frame (50Hz) */
#define CS_NS        10000000ull

extern const struct roger_config *rg;     /* the game's configuration */

/* main.c */
extern int rg_interactive, rg_full_os, rg_trace_gaps;
extern uint64_t rg_limit, rg_t0_ns;
extern const char *rg_appdir, *rg_savedir;
void ev(const char *name, const char *fmt, ...);
void finish(const char *why) __attribute__((noreturn));
void script_run(void);
int  script_active(void);

/* cpu.c */
extern uint32_t bank_r13[4], bank_r14[4];
extern int in_handler, interp_only;
extern uint32_t g_cur_pc;
extern uint64_t interp_count, smc_count;
void call_code(uint32_t addr, uint32_t mode, uint32_t r14);
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
void cpu_programs_loaded(const char *leaf, uint32_t addr);   /* OS_File loaded a file */

/* irq.c */
extern uint32_t monotonic, events_enabled;
void recompute_irq(void);
void rt_service(int vsync_now);
void wait_vsync(void);
void call_vector(unsigned vec, int nregs, const uint32_t *regs);
int  irq_swi(uint32_t b);                 /* OS_Claim/Release, device vectors */

/* video.c */
struct rg_mode { int width, height, bpp, xeig, yeig, text_cols, text_rows; };
extern int screen_mode;
extern uint32_t bank, display_bank;
const struct rg_mode *mode_info(int m);   /* NULL if not a known mode */
const struct rg_mode *cur_mode(void);
uint32_t screen_base(void);               /* start of screen memory */
uint32_t screen_size(void);               /* total screen memory */
uint32_t mode_bytes(const struct rg_mode *m);
uint32_t write_bank_addr(void);           /* screen memory the VDU drivers write to */
void vdu_put(uint8_t c);
const uint8_t *displayed(void);           /* the frame as the display hardware shows it */
uint32_t vduvar(uint32_t v);
uint32_t modevar(int mode, uint32_t v);
int  video_os_word(uint32_t r0, uint8_t *blk);   /* OS_Word 12/21/22 */
int  video_timer_line(uint32_t latch);    /* output line where timer 1 (started at flyback) fires */
void video_set_bank(int which, uint32_t b);
uint32_t vidc_rgb(int line, uint8_t pixel);   /* host colour of a pixel on an output line */
void video_default_palette(void);         /* the palette a mode change sets */
void video_save_vdu(FILE *f);             /* the VDU stream so far */
extern int mbox[4];                       /* OS_Word 21,1 mouse bounding box */

/* vdu.c: the VDU drivers (interactive runs) */
extern unsigned vdu_unhandled;
void vdu_write(uint8_t c);
int  sprite_op_full(void);

/* sound.c */
void sound_dma_advance(uint32_t us);
void sound_tick(void);                    /* every centisecond */
int  sound_swi(uint32_t b);

/* audio.c: host audio */
int  audio_init_device(void);
int  audio_init_wav(const char *path);
void audio_close(void);

/* files.c */
extern int swi_x;                         /* the current SWI is X-flagged */
int  cstr(uint32_t a, char *out, int term_ctrl);
int  hostpath(const char *name, char *out);
int  hostpath_read(const char *name, char *out);   /* preferring a saved copy */
void file_info(const char *path, uint32_t *load, uint32_t *exec);
int  save_file(const char *name, uint32_t addr, uint32_t len);
long load_file(const char *path, uint32_t addr);
int  files_swi(uint32_t b);
void sprite_op(void);

/* modules.c */
void modules_load(void);
int  modules_rmload(const char *name);   /* 0: not a configured module file */
int  modules_rmkill(const char *title);
int  modules_swi(uint32_t b, int x);
int  modules_gap(uint32_t a);             /* an untranslated address in a module */

/* cli.c: OS_CLI and system variables */
void cli_init(void);
void gstrans(const char *in, char *out, int size);
int  cli_swi(uint32_t b, int x);

/* swi.c */
extern uint32_t swi_pc;
int  pressed(int inkey);

/* host.c: the SDL front end */
int  display_init(int scale, const char *fullscreen, int crt);   /* fullscreen: NULL, "" or "WxH[@Hz]" */
void display_toggle_fullscreen(void);
void display_list_modes(void);
void display_present(const uint8_t *pixels);
void display_poll(void);
uint64_t display_ticks_ns(void);
void display_sleep_ns(uint64_t ns);
void display_set_mouse(int x, int y);
void display_inject_tap(int px, int py);
void display_screenshot(const uint8_t *pix, const char *path);
extern uint8_t key_down[128];             /* by RISC OS internal key number */
extern int mouse_x, mouse_y, mouse_buttons;   /* OS units; 4 select, 2 menu, 1 adjust */
extern int quit_requested, escape_pressed, screenshot_requested;
int  key_buffer_get(void);
void key_buffer_put(int c);
void key_buffer_flush(void);

#endif
