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
static uint64_t next_tick = TICK_STEPS, next_vsync = VSYNC_STEPS;
static uint64_t next_tick_ns, next_vsync_ns;
static unsigned click_i;
static int last_buttons, shots;
volatile uint64_t rg_last_service_ns;      /* watchdog: last time rt_service() ran a vsync */

void irq_start_clock(uint64_t now) { next_tick_ns = next_vsync_ns = now; }

void recompute_irq(void)
{
    uint64_t n = next_tick < next_vsync ? next_tick : next_vsync;
    if (!rg_interactive && click_i < rg->nclicks && rg->clicks[click_i].icount < n) n = rg->clicks[click_i].icount;
    if (rg_limit < n) n = rg_limit;
    irq_next = n;
}

/* ---------------------------------------------------------------- vectors */
static struct { uint32_t code, ws; int set; } vectors[0x20];

void call_vector(unsigned vec, int nregs, const uint32_t *regs)
{
    /* Events only reach EventV once enabled with OS_Byte 14 (a handler that
     * doesn't check the event number would otherwise see every one). */
    if (vec == 0x10 && !(events_enabled & (1u << (regs[0] & 31)))) return;
    if (vec >= 0x20 || !vectors[vec].set) return;
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, snpc = npc;
    memcpy(saved, R, sizeof R);
    for (int k = 0; k < nregs; k++) R[k] = regs[k];
    R[12] = vectors[vec].ws;
    /* R14 passes the call on; a claimant that intercepts it instead pulls
     * the kernel's return address off the stack (ldmfd sp!, {pc}). The
     * handler runs on the interrupted code's stack, which may be ascending
     * (Galactic Dan), so the word under it is put back afterwards. */
    R[14] = HANDLER_RET;
    R[13] -= 4;
    uint32_t under_sp = R[13], under = RD32(under_sp);
    WR32(under_sp, HANDLER_RET);
    npc = vectors[vec].code;
    in_handler = 1;
    COV_ASYNC_ENTER();
    run_until(HANDLER_RET);
    COV_ASYNC_EXIT();
    in_handler = 0;
    WR32(under_sp, under);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    npc = snpc;
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
 * (&3200014) saying which interrupt this is. */
static void call_irq1v(uint32_t irq_bit)
{
    uint32_t a = RD32(0x100);
    if (!a || !cpu_translated(a)) return;
    WR32(IOC + 0x14, irq_bit);
    WR32(IOC + 0x24, 0);
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, snpc = npc;
    memcpy(saved, R, sizeof R);
    R[14] = HANDLER_RET;
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
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    npc = snpc;
}

/* One frame's interrupts: vsync (VIDC flyback) first, then IOC timer 1 if
 * the program has it running - the usual way to change the palette part
 * way down the frame. The display gets the palette as the vsync handlers
 * left it above the line where the timer fires, and as the timer handler
 * left it below. */
static void frame_irq(void)
{
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
}

/* ---------------------------------------------------------------- SWIs */
int irq_swi(uint32_t b)
{
    switch (b) {
    case 0x4B: case 0x4C:                        /* OS_ClaimDeviceVector / OS_ReleaseDeviceVector */
        ev(b == 0x4B ? "OS_ClaimDeviceVector" : "OS_ReleaseDeviceVector", "iii", R[0], R[1], R[2]);
        if (R[0] < 16) {
            if (b == 0x4B) { devices[R[0]].code = R[1]; devices[R[0]].ws = R[2]; }
            else if (devices[R[0]].code == R[1]) devices[R[0]].code = 0;
        }
        return 1;
    case 0x1F: case 0x20:                        /* OS_Claim / OS_Release */
        ev(b == 0x1F ? "OS_Claim" : "OS_Release", "iii", R[0], R[1], R[2]);
        if (b == 0x1F && R[0] < 0x20) {
            vectors[R[0]].code = R[1]; vectors[R[0]].ws = R[2]; vectors[R[0]].set = 1;
        } else if (b == 0x20 && R[0] < 0x20 && vectors[R[0]].set &&
                   vectors[R[0]].code == R[1] && vectors[R[0]].ws == R[2])
            vectors[R[0]].set = 0;               /* e.g. a module's finalisation */
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the clock */
static void centisecond(void)
{
    sound_dma_advance(10000);                    /* 1 cs of sound DMA */
    sound_tick();
    if (rg->centisecond) rg->centisecond();
    call_vector(0x1C, 0, 0);                     /* TickerV */
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

static void vsync(void)
{
    monotonic += 2;
    frame_irq();
    if (rg->frame) rg->frame();
    uint32_t r[2] = {4, 0};                      /* event 4: vsync */
    call_vector(0x10, 2, r);
    key_events();
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
    if (vsync_now || now >= next_vsync_ns) {
        next_vsync_ns += 2 * CS_NS;
        display_poll();
        if (script_active()) script_run();
        if (quit_requested) finish("window closed");
        rg_last_service_ns = now;
        vsync();
        if (mouse_buttons != last_buttons) {
            last_buttons = mouse_buttons;
            ev("mouse", "iii", mouse_x, mouse_y, mouse_buttons);
            uint32_t m[5] = {10, mouse_x, mouse_y, mouse_buttons, monotonic};   /* event 10: mouse */
            call_vector(0x10, 5, m);
        }
        /* Present last: what the program does at flyback (palette pushes,
         * bank flips, pointer redraw in its vsync event) shows in the frame
         * scanned out right after it. */
        if (screenshot_requested) {
            char fn[64];
            snprintf(fn, sizeof fn, "screenshot-%d.bmp", ++shots);
            display_screenshot(displayed(), fn);
            fprintf(stderr, "saved %s\n", fn);
            screenshot_requested = 0;
        }
        display_present(displayed());
    }
}

void wait_vsync(void)
{
    uint64_t now = display_ticks_ns();
    if (now < next_vsync_ns) display_sleep_ns(next_vsync_ns - now);
    rt_service(1);
}

void irq_point(void)
{
    if (in_handler) return;
    if (!rg_interactive && icount >= rg_limit) finish("instruction limit");
    if (MODEBITS & 0x08000000u) {               /* IRQs disabled: they wait */
        irq_next = icount + 64;
        return;
    }
    if (rg_interactive) {
        rt_service(0);
        irq_next = icount + 20000;
        return;
    }
    if (icount >= rg_limit) finish("instruction limit");
    if (icount >= next_tick) {
        next_tick += TICK_STEPS;
        centisecond();
    }
    if (icount >= next_vsync) {
        next_vsync += VSYNC_STEPS;
        vsync();
    }
    if (click_i < rg->nclicks && icount >= rg->clicks[click_i].icount) {
        uint32_t x = rg->clicks[click_i].x, y = rg->clicks[click_i].y, b = rg->clicks[click_i].buttons;
        click_i++;
        ev("mouse", "iii", x, y, b);
        uint32_t r[5] = {10, x, y, b, monotonic};
        call_vector(0x10, 5, r);
    }
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
