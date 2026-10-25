/* Time and interrupts: the centisecond ticker, the 50Hz vsync (VIDC
 * flyback), IOC timer 1, and what the program hooks them with - vectors
 * (OS_Claim), events (EventV), device vectors (OS_ClaimDeviceVector) and,
 * optionally, a raw IRQ handler at &100.
 *
 * Interactive runs follow the host clock: rt_service() is called from the
 * instruction stream every 20000 instructions and catches up with real
 * time. Headless runs (--limit) are deterministic: ticks every TICK_STEPS
 * instructions and vsyncs every VSYNC_STEPS, with scripted input at fixed
 * instruction counts (roger_config.clicks/keys). */
#include <string.h>
#include "internal.h"

uint32_t monotonic, events_enabled;
uint64_t tick_steps = 20000;
static uint64_t next_tick = 20000, next_vsync = 40000;
static uint64_t next_tick_ns, next_vsync_ns;
static uint64_t snd_icount, snd_ns;       /* the sound DMA clock (sound_level0_time) */
static unsigned click_i;
static int present_pending;               /* roger_config.present_after_flyback: the picture due */
static uint64_t present_due_ns, present_due_icount;
volatile uint64_t rg_last_service_ns;      /* watchdog: last time rt_service() ran a vsync */

void irq_start_clock(uint64_t now) { next_tick_ns = next_vsync_ns = now; }

/* Headless runs: the CPU's speed, n million instructions a second (before
 * the program starts) */
void irq_headless_mips(float n)
{
    tick_steps = (uint64_t)(n * 10000.0f + 0.5f);
    if (tick_steps < 100) tick_steps = 100;
    next_tick = TICK_STEPS; next_vsync = VSYNC_STEPS;
}

void recompute_irq(void)
{
    uint64_t n = next_tick < next_vsync ? next_tick : next_vsync;
    if (!rg_interactive && click_i < scenario_clicks() && rg->clicks[click_i].icount < n) n = rg->clicks[click_i].icount;
    if (!rg_interactive && present_pending && present_due_icount < n) n = present_due_icount;
    if (!rg_interactive && irq_sound_claimed()) {    /* the sound buffer's end */
        uint32_t due = sound_level0_due_us();
        uint64_t at = snd_icount + ((uint64_t)due * TICK_STEPS + 9999) / 10000;
        if (due && at < n) n = at;
    }
    if (rg_limit < n) n = rg_limit;
    irq_next = n;
}

/* ---------------------------------------------------------------- vectors */
/* Each vector's claimants, the one claimed last first: RISC OS calls them in
 * turn while each passes the call on. */
#define MAXCLAIMS 8
static struct { uint32_t code, ws; } vectors[0x20][MAXCLAIMS];
static int nclaims[0x20];

static void vector_release(unsigned vec, uint32_t code, uint32_t ws)
{
    for (int k = 0; k < nclaims[vec]; k++)
        if (vectors[vec][k].code == code && vectors[vec][k].ws == ws) {
            memmove(vectors[vec] + k, vectors[vec] + k + 1, (--nclaims[vec] - k) * sizeof vectors[vec][0]);
            return;
        }
}

static void vector_claim(unsigned vec, uint32_t code, uint32_t ws)
{
    vector_release(vec, code, ws);               /* claiming again moves it to the front */
    if (nclaims[vec] == MAXCLAIMS) { fprintf(stderr, "vector &%X: too many claimants\n", vec); return; }
    memmove(vectors[vec] + 1, vectors[vec], nclaims[vec]++ * sizeof vectors[vec][0]);
    vectors[vec][0].code = code; vectors[vec][0].ws = ws;
}

void call_vector(unsigned vec, int nregs, const uint32_t *regs)
{
    /* Events only reach EventV once enabled with OS_Byte 14 (a handler that
     * doesn't check the event number would otherwise see every one). */
    if (vec == 0x10 && !(events_enabled & (1u << (regs[0] & 31)))) return;
    if (vec >= 0x20 || !nclaims[vec]) return;
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, snpc = npc, sr13 = bank_r13[2];
    memcpy(saved, R, sizeof R);
    for (int k = 0; k < nregs; k++) R[k] = regs[k];
    /* These are the vectors RISC OS calls from its interrupt handler
     * (TickerV, EventV for vsync, key and mouse events): in IRQ mode, on
     * the IRQ stack. The interrupted code's R13 is left alone - it may not
     * be a stack at all (Stunt Racer 2000's text plotter walks its text
     * with SP). R14 passes the call on, to the claimant before it, with the
     * registers as it leaves them; a claimant that intercepts it instead
     * pulls the return address off the stack (ldmfd sp!, {pc}). */
    int irq = (MODEBITS & 3) == 2;
    uint32_t sp = (irq ? R[13] : IRQ_STACK) - 4;
    for (int k = 0; k < nclaims[vec]; k++) {
        WR32(sp, HANDLER_RET | MODE_IRQ);
        if (irq) R[13] = sp; else bank_r13[2] = sp;
        R[12] = vectors[vec][k].ws;
        call_code(vectors[vec][k].code, MODE_IRQ, HANDLER_RET | MODE_IRQ);
        if ((irq ? R[13] : bank_r13[2]) != sp) break;   /* intercepted */
    }
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    npc = snpc;
    bank_r13[2] = sr13;
}

/* ---------------------------------------------------------------- device vectors
 * OS_ClaimDeviceVector handlers, called as the kernel's IRQ dispatcher
 * would: IRQ mode, R3 = IOC, R12 = the workspace given at claim time,
 * return to R14. */
#define DEV_VSYNC    3
#define DEV_TIMER1   6
#define DEV_SOUND    9
static struct { uint32_t code, ws; } devices[16];
static uint32_t timer1_latch;

static void call_device(int dev, uint32_t irq_bit)
{
    if (!devices[dev].code) return;
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    if (dev < 8) MEM[IOC + 0x10] = MEM[IOC + 0x14] = irq_bit;   /* IRQ status / request A */
    else MEM[IOC + 0x20] = MEM[IOC + 0x24] = irq_bit;           /* B (podules, sound...) */
    R[3] = IOC; R[12] = devices[dev].ws;
    bank_r13[2] = IRQ_STACK;                          /* each interrupt starts on an empty IRQ stack */
    call_code(devices[dev].code, MODE_IRQ, HANDLER_RET | MODE_IRQ);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
}

/* Device 9: the sound DMA buffer interrupt (IRQ B bit 1), for a program
 * that drives the sound DMA itself (sound.c) */
int irq_sound_claimed(void) { return devices[DEV_SOUND].code != 0; }
void irq_sound(void) { call_device(DEV_SOUND, 0x02); }

/* The raw handler at &100 (roger_config.irq1v), with IOC's IRQ request A
 * (&3200014) saying which interrupt this is. It is entered as the IRQ
 * vector enters it: in IRQ mode, on an empty IRQ stack, R14 holding the
 * mode to return to (a handler that returns with movs pc,lr while the
 * interrupted code is in SVC mode - a SWI - would otherwise leave the CPU
 * in USR mode with the SVC stack as R13: James Pond, in MusicModule's
 * SWIs). The interrupted mode is restored afterwards. */
static void call_irq1v(uint32_t irq_bit)
{
    uint32_t a = RD32(0x100);
    if (!a || !cpu_translated(a)) return;
    WR32(IOC + 0x14, irq_bit);
    WR32(IOC + 0x24, 0);
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, snpc = npc, smode = MODEBITS;
    memcpy(saved, R, sizeof R);
    bank_r13[2] = IRQ_STACK;
    set_psr_mode(MODE_IRQ);
    R[14] = HANDLER_RET | MODE_IRQ;
    npc = a;
    in_handler = 1;
    COV_ASYNC_ENTER();
    uint64_t i0 = icount;
    while (npc != HANDLER_RET) {
        if (npc == RETURN_TO_OS) finish("returned to OS");
        if (icount - i0 > 5000000) {
            fprintf(stderr, "IRQ handler %08x: bailing out after 5M instructions, pc=%08x\n", a, npc);
            break;
        }
        call_addr(npc);
    }
    COV_ASYNC_EXIT();
    in_handler = 0;
    set_psr_mode(smode);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    npc = snpc;
}

/* One frame's interrupts: vsync (VIDC flyback) first, then IOC timer 1 if
 * the program has it running - the usual way to change the palette part
 * way down the frame. The display gets the palette as the vsync handlers
 * left it above the line where the timer fires, and as the timer handler
 * left it below. With roger_config.raster_palette the timer goes on firing
 * down the frame, every latch + 1 ticks (the latch as the handler leaves
 * it), and each line it fires on gets the palette its handler left: colour
 * bars. */
static void frame_irq(void)
{
    if (rg->raster_palette) video_raster_start();
    uint32_t written = MEM[IOC + 0x50] | MEM[IOC + 0x54] << 8;
    if (written) timer1_latch = written;          /* programmed since the last frame */
    MEM[IOC + 0x50] = MEM[IOC + 0x54] = 0;
    call_device(DEV_VSYNC, 0x08);
    if (rg->irq1v) call_irq1v(0x08);
    memcpy(vidc_palette_top, vidc_palette, sizeof vidc_palette_top);
    /* timer 1 latch (low &50, high &54), counting at 2MHz: 128 ticks a line */
    uint32_t latch = MEM[IOC + 0x50] | MEM[IOC + 0x54] << 8;
    if (!latch) latch = timer1_latch;
    int running = latch && (MEM[IOC + 0x18] & 0x40);
    if (rg->irq1v && (running || rg->irq1v_timer_always)) call_irq1v(0x40);
    if (running && devices[DEV_TIMER1].code) call_device(DEV_TIMER1, 0x40);
    else if (!(rg->irq1v && running)) { palette_split = 0; return; }
    palette_split = video_timer_line(latch);
    if (!rg->raster_palette) return;
    uint32_t ticks = latch + 1;                   /* since flyback */
    video_raster_add(video_raster_line(ticks));
    for (int k = 0; k < 1024; k++) {
        uint32_t w = MEM[IOC + 0x50] | MEM[IOC + 0x54] << 8;
        if (w) { latch = w; MEM[IOC + 0x50] = MEM[IOC + 0x54] = 0; }
        if (!(MEM[IOC + 0x18] & 0x40)) break;     /* the timer's interrupt masked off */
        ticks += latch + 1;
        int line = video_raster_line(ticks);
        if (line >= 256) break;
        if (rg->irq1v) call_irq1v(0x40);
        if (devices[DEV_TIMER1].code) call_device(DEV_TIMER1, 0x40);
        video_raster_add(line);
    }
    timer1_latch = latch;                         /* the next frame's, unless the vsync handlers change it */
}

/* ---------------------------------------------------------------- SWIs */
/* ticker events (OS_CallAfter, OS_CallEvery: see centisecond) */
#define MAXTICKER 32
static struct { uint32_t left, every, code, r12; } ticker[MAXTICKER];
static int nticker;

int irq_swi(uint32_t b)
{
    switch (b) {
    case 0x3B: case 0x3C:                        /* OS_CallAfter / OS_CallEvery */
        if (!R[0] || R[0] >= 0xFFFFFC00u) { os_error(0x1F7, "Invalid time interval"); return 1; }
        if (nticker == MAXTICKER) { os_error(0x101, "No room for another ticker event"); return 1; }
        ticker[nticker].left = b == 0x3B ? R[0] : R[0] + 1;
        ticker[nticker].every = b == 0x3B ? 0 : R[0] + 1;
        ticker[nticker].code = R[1] & 0x3FFFFFC; ticker[nticker++].r12 = R[2];
        ev(b == 0x3B ? "OS_CallAfter" : "OS_CallEvery", "iii", R[0], R[1], R[2]);
        return 1;
    case 0x3D:                                   /* OS_RemoveTickerEvent */
        for (int k = 0; k < nticker; k++)
            if (ticker[k].code == (R[0] & 0x3FFFFFC) && ticker[k].r12 == R[1]) ticker[k--] = ticker[--nticker];
        return 1;
    case 0x4B: case 0x4C:                        /* OS_ClaimDeviceVector / OS_ReleaseDeviceVector */
        ev(b == 0x4B ? "OS_ClaimDeviceVector" : "OS_ReleaseDeviceVector", "iii", R[0], R[1], R[2]);
        if (R[0] < 16) {
            if (b == 0x4B) { devices[R[0]].code = R[1]; devices[R[0]].ws = R[2]; }
            else if (devices[R[0]].code == R[1]) devices[R[0]].code = 0;
        }
        return 1;
    case 0x1F: case 0x20:                        /* OS_Claim / OS_Release */
        ev(b == 0x1F ? "OS_Claim" : "OS_Release", "iii", R[0], R[1], R[2]);
        if (R[0] < 0x20) {
            if (b == 0x1F) vector_claim(R[0], R[1], R[2]);
            else vector_release(R[0], R[1], R[2]);    /* e.g. a module's finalisation */
        }
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the clock */
/* Ticker events: OS_CallAfter (R0 centiseconds, then once) and OS_CallEvery
 * (every R0 + 1: callers give one less), R1 the routine, R2 its R12;
 * OS_RemoveTickerEvent (R0, R1 as given). Called in SVC mode, interrupts
 * disabled, as RISC OS's TickEvents does, each centisecond tick */

static void ticker_events(void)
{
    for (int k = 0; k < nticker; k++) {
        if (--ticker[k].left) continue;
        uint32_t code = ticker[k].code, r12 = ticker[k].r12;
        if (ticker[k].every) ticker[k].left = ticker[k].every;
        else { ticker[k--] = ticker[--nticker]; }
        uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, snpc = npc;
        memcpy(saved, R, sizeof R);
        R[12] = r12;
        call_code(code, MODE_SVC | 0x08000000u, HANDLER_RET | MODE_SVC | 0x08000000u);
        memcpy(R, saved, sizeof R);
        NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
        npc = snpc;
    }
}

/* IOC timer 0 is the OS's centisecond ticker: counting down at 2MHz from
 * 20000 (RISC OS's latch, &4E20), interrupting and reloading as it reaches
 * zero. A program reads its count by writing the latch command (&320004C)
 * and then &3200040 (low) and &3200044 (high): the time since the last
 * tick, at a finer grain (Desktop Tracker times its music by it). Each
 * tick also counts in RISC OS 3's zero page word at &A38, which such
 * programs read alongside (their OS_Byte 129 says RISC OS 3), and in
 * MetroGnome at &10C (RISC OS 2 and 3's, read directly by programs of
 * their time: Tertis' music) */
#define ZP_CENTISECONDS 0xA38u
#define ZP_METROGNOME 0x10Cu
static uint32_t centiseconds;
void ioc_latch_timer0(void)
{
    uint64_t t = rg_interactive ? (display_ticks_ns() + CS_NS - next_tick_ns) / 500     /* (2MHz: 500ns) */
                                : (icount + TICK_STEPS - next_tick) * 20000 / TICK_STEPS;
    uint32_t count = t >= 20000 ? 0 : 20000 - (uint32_t)t;
    MEM[IOC + 0x40] = (uint8_t)count;
    MEM[IOC + 0x44] = (uint8_t)(count >> 8);
}

/* A program driving the sound DMA (device 9) has its buffer interrupt when
 * the buffer ends, as MEMC gives it, not a centisecond's worth at once:
 * a player that times its interrupts (Desktop Tracker: IOC timer 0, its
 * intervals averaged) would otherwise see them in pairs, 0 and 10ms
 * apart. The DMA clock follows emulated time (instructions, headless) or
 * the host's, from where it was last advanced; the OS's own SoundDMA, and
 * a Level 1 handler, still go a centisecond at a time. */
static void sound_level0_time(void)
{
    uint64_t now = rg_interactive ? display_ticks_ns() : 0;
    if (!irq_sound_claimed()) { snd_icount = icount; snd_ns = now; return; }
    uint64_t us;
    if (rg_interactive) {
        us = (now - snd_ns) / 1000;
        if (us > 200000) { snd_ns = now; return; }  /* a stall: don't burst */
        snd_ns += us * 1000;
    } else {
        us = (icount - snd_icount) * 10000 / TICK_STEPS;
        snd_icount += us * TICK_STEPS / 10000;
    }
    if (us) sound_dma_advance((uint32_t)us);
}

static void centisecond(void)
{
    WR32(ZP_CENTISECONDS, ++centiseconds);
    WR32(ZP_METROGNOME, centiseconds);
    if (!irq_sound_claimed()) sound_dma_advance(10000);   /* 1 cs of sound DMA */
    sound_tick();
    if (rg->centisecond) rg->centisecond();
    call_vector(0x1C, 0, 0);                     /* TickerV */
    if (nticker) ticker_events();
}

/* Event 11 (key transition): R1 0 up / 1 down, R2 the low-level key
 * number, R3 the keyboard driver. Key states are compared once a frame;
 * the table maps internal key numbers (INKEY -(n+1)) to low-level ones. */
static const uint8_t lowlevel[128] = {
    [3] = 0x4C, [4] = 0x3B, [5] = 0x5E, [6] = 0x58, [7] = 0x61, [8] = 0x60,
    [9] = 0x70, [10] = 0x71, [11] = 0x72,
    [16] = 0x27, [17] = 0x13, [18] = 0x14, [19] = 0x15, [20] = 0x04, [21] = 0x18, [22] = 0x07,
    [23] = 0x1B, [25] = 0x62, [26] = 0x4A, [27] = 0x37, [28] = 0x0B, [29] = 0x0C, [30] = 0x0A,
    [31] = 0x0E, [32] = 0x0D, [33] = 0x28, [34] = 0x29, [35] = 0x2B, [36] = 0x17, [37] = 0x2E,
    [38] = 0x19, [39] = 0x1A, [41] = 0x63, [42] = 0x38, [43] = 0x39, [44] = 0x0F, [45] = 0x10,
    [46] = 0x1D, [47] = 0x1E, [48] = 0x11, [49] = 0x12, [50] = 0x3E, [51] = 0x2A, [52] = 0x16,
    [53] = 0x2D, [54] = 0x2F, [55] = 0x30, [56] = 0x31, [57] = 0x59, [58] = 0x4B, [59] = 0x3A,
    [60] = 0x67, [61] = 0x1F, [62] = 0x20, [63] = 0x21, [64] = 0x5D, [65] = 0x3C, [66] = 0x4F,
    [67] = 0x3F, [68] = 0x2C, [69] = 0x42, [70] = 0x43, [73] = 0x47, [74] = 0x23, [76] = 0x66,
    [77] = 0x22, [78] = 0x36, [79] = 0x46, [81] = 0x3D, [82] = 0x50, [83] = 0x40, [84] = 0x41,
    [85] = 0x53, [86] = 0x44, [87] = 0x45, [88] = 0x32, [89] = 0x34, [90] = 0x25, [91] = 0x24,
    [93] = 0x1C, [96] = 0x26, [97] = 0x4E, [98] = 0x5F, [99] = 0x51, [100] = 0x52, [101] = 0x54,
    [102] = 0x55, [103] = 0x56, [104] = 0x57, [105] = 0x35, [106] = 0x65, [107] = 0x5A,
    [108] = 0x5C, [112] = 0x00, [113] = 0x01, [114] = 0x02, [115] = 0x03, [116] = 0x05,
    [117] = 0x06, [118] = 0x08, [119] = 0x09, [120] = 0x33, [121] = 0x64, [122] = 0x48,
    [123] = 0x49, [124] = 0x5B,
};
static void key_events(void)
{
    static uint8_t was[128];
    if (!(events_enabled & (1u << 11))) return;
    for (int k = 3; k < 128; k++) {
        if (!lowlevel[k] && k != 112) continue;
        /* a scripted Shift/Ctrl/Alt (internal 0-2, either side) is the left one */
        int down = pressed(k + 1) || (!rg_interactive && k <= 5 && pressed(k - 2));
        if (down == was[k]) continue;
        was[k] = down;
        uint32_t r[4] = {11, (uint32_t)down, lowlevel[k], 0};
        call_vector(0x10, 4, r);
    }
}

uint8_t vsync_counter;                     /* OS_Byte 176: down by one each vsync */
static void vsync(void)
{
    monotonic += 2;
    interval_tick();
    vsync_counter--;
    video_pointer_frame();                       /* the pointer image, as the cursor DMA reads it */
    frame_irq();
    if (rg->frame) rg->frame();
    /* event 4: vsync, raised from the kernel's VSync interrupt handler,
     * which is entered as device handlers are, with R3 = IOC: a claimant may
     * clear the interrupt itself (Diggers' intro: strb r1,[r3,#&14], with
     * the program's R3 a store into MEMC's control register that turned the
     * sound DMA off) */
    uint32_t r[4] = {4, 0, R[2], IOC};
    call_vector(0x10, 4, r);
    key_events();
}

/* After a vsync: a mouse button change (event 10), and a screenshot asked
 * for. (Presenting comes last: what the program does at flyback - palette
 * pushes, bank flips, pointer redraw in its vsync event - shows in the
 * frame scanned out right after it.) */
static void shot(void)
{
    static int shots;
    if (!screenshot_requested) return;
    char fn[64];
    snprintf(fn, sizeof fn, "screenshot-%d.bmp", ++shots);
    if (desktop_visible()) desktop_screenshot(fn);
    else display_screenshot(displayed(), fn);
    fprintf(stderr, "saved %s\n", fn);
    screenshot_requested = 0;
}

static void mouse_and_shots(void)
{
    static int last_buttons;
    if (mouse_buttons != last_buttons) {
        last_buttons = mouse_buttons;
        ev("mouse", "iii", mouse_x, mouse_y, mouse_buttons);
        uint32_t m[5] = {10, mouse_x, mouse_y, mouse_buttons, monotonic};   /* event 10: mouse */
        call_vector(0x10, 5, m);
        if (rg_desktop) desktop_buttons_changed(mouse_x, mouse_y, mouse_buttons);
    }
    if (!rg->present_after_flyback) shot();
}

/* roger_config.present_after_flyback: the picture (and a screenshot) is
 * taken when the next frame's display starts, the flyback after vsync,
 * so that what the program sets in the flyback - a bank flip or display
 * start after its vsync wait, with the fine scroll its vsync handler set -
 * makes the same picture, as on the machine. Otherwise it is taken at
 * vsync, once the vsync handlers have run. */
static void present(void)
{
    present_pending = 0;
    shot();
    if (!rg_interactive) return;
    if (desktop_visible()) desktop_present();
    else display_present(displayed());
}

static void present_after_flyback(uint64_t now)
{
    present_pending = 1;
    present_due_ns = now + (uint64_t)video_flyback_lines() * 64000;     /* 64us a line */
    present_due_icount = icount + (uint64_t)video_flyback_lines() * VSYNC_STEPS / 312;
}

void rt_service(int vsync_now)
{
    uint64_t now = display_ticks_ns();
    if (now > next_tick_ns + 20 * CS_NS) next_tick_ns = now;       /* don't burst after stalls */
    if (now > next_vsync_ns + 20 * CS_NS) next_vsync_ns = now;
    while (now >= next_tick_ns) {
        next_tick_ns += CS_NS;
        centisecond();
    }
    int vsync_due = vsync_now || now >= next_vsync_ns;
    if (present_pending && (now >= present_due_ns || vsync_due)) present();
    if (vsync_due) {
        next_vsync_ns += 2 * CS_NS;
        display_poll();
        if (script_active()) script_run();
        if (quit_requested) finish("window closed");
        rg_last_service_ns = now;
        vsync();
        mouse_and_shots();
        if (rg->present_after_flyback) present_after_flyback(now);
        else present();
    }
}

void wait_vsync(void)
{
    uint64_t now = display_ticks_ns();
    if (present_pending && now < present_due_ns) {
        display_sleep_ns(present_due_ns - now);
        rt_service(0);
        now = display_ticks_ns();
    }
    if (now < next_vsync_ns) display_sleep_ns(next_vsync_ns - now);
    rt_service(1);
}

/* Interactive runs at rg_mips million instructions a second (if set): an
 * instruction count due at a time; ahead of it, sleep (a game that never
 * waits for the frame runs at the original machine's speed). Falling far
 * behind (the host stalled, or the program waited for something) starts
 * the count afresh rather than bursting to catch up. */
float rg_mips;
static void throttle(void)
{
    static uint64_t base_icount, base_ns;
    if (rg_mips <= 0) return;
    uint64_t now = display_ticks_ns();
    if (!base_ns) { base_ns = now; base_icount = icount; return; }
    uint64_t due = base_ns + (uint64_t)((double)(icount - base_icount) * 1000.0 / rg_mips);
    if (due > now) display_sleep_ns(due - now > 50000000ull ? 50000000ull : due - now);
    else if (now - due > 50000000ull) { base_ns = now; base_icount = icount; }
}

void irq_point(void)
{
    if (in_handler) return;
    pchist_sample();
    if (!rg_interactive && icount >= rg_limit) finish("instruction limit");
    if (MODEBITS & 0x08000000u) {               /* IRQs disabled: they wait */
        irq_next = icount + 64;
        return;
    }
    sound_level0_time();
    if (rg_interactive) {
        rt_service(0);
        throttle();
        irq_next = icount + 20000;
        run_callbacks();
        return;
    }
    if (icount >= rg_limit) finish("instruction limit");
    if (icount >= next_tick) {
        next_tick += TICK_STEPS;
        centisecond();
    }
    if (present_pending && icount >= present_due_icount) present();
    if (icount >= next_vsync) {
        next_vsync += VSYNC_STEPS;
        if (present_pending) present();
        vsync();
        if (script_active()) { script_run(); mouse_and_shots(); }
        if (rg->present_after_flyback) present_after_flyback(0);
    }
    if (click_i < scenario_clicks() && icount >= rg->clicks[click_i].icount) {
        uint32_t x = rg->clicks[click_i].x, y = rg->clicks[click_i].y, b = rg->clicks[click_i].buttons;
        click_i++;
        ev("mouse", "iii", x, y, b);
        uint32_t r[5] = {10, x, y, b, monotonic};
        call_vector(0x10, 5, r);
    }
    run_callbacks();
    recompute_irq();
}

/* Host code standing in for the program (a translated BASIC program
 * polling TIME, INKEY or the mouse): count n instructions' worth of time
 * and take what falls due - ticks, vsyncs and scripted input when
 * headless, the display and host events when interactive. */
void roger_idle(unsigned n)
{
    icount += n;
    irq_point();
}
