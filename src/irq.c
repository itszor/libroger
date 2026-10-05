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
static struct { uint32_t code, ws; } devices[16];
static uint32_t timer1_latch;

static void call_device(int dev, uint32_t irq_bit)
{
    if (!devices[dev].code) return;
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    MEM[IOC + 0x10] = MEM[IOC + 0x14] = irq_bit;       /* IRQ status / request A */
    R[3] = IOC; R[12] = devices[dev].ws;
    bank_r13[2] = IRQ_STACK;                          /* each interrupt starts on an empty IRQ stack */
    call_code(devices[dev].code, MODE_IRQ, HANDLER_RET | MODE_IRQ);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
}

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

static void vsync(void)
{
    monotonic += 2;
    frame_irq();
    if (rg->frame) rg->frame();
    uint32_t r[2] = {4, 0};                      /* event 4: vsync */
    call_vector(0x10, 2, r);
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
