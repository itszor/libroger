/* The RISC OS sound system.
 *
 * Level 0 (DMA): every buffer period (samples x period us) the sound DMA
 * interrupt calls the Level 1 handler's fill code (word 0 of the handler
 * block given to Sound_Configure) with R12 = buffer start, R10 = buffer
 * end, R11 = number of channels (samples interleaved, stride R11), R9 = the
 * handler block. The bytes are VIDC 8-bit logarithmic samples, one per
 * channel per frame, panned by Sound_Stereo. The DMA runs in step with
 * emulated time (sound_dma_advance(), every centisecond) and each buffer is
 * converted to 16-bit stereo for the host (audio.c: SDL stream or --wav).
 *
 * The OS's own Level 1 handler block (what Sound_Configure returns in R3
 * until something else is installed) is real memory: +0 fill code, +4
 * overrun fix-up, +8 the linear-to-log table (8192 bytes: a 13-bit two's
 * complement sample -> VIDC log byte), +12 the log amplitude scale table
 * (256 bytes: a log amplitude * 2 -> scaled amplitude * 2, here the
 * identity at full volume), which voice generators use.
 *
 * Level 1 (SoundChannels), unless a program or module installs its own:
 * per channel, the attached voice generator fills its interleaved samples.
 * Voice generator header (Sound_InstallVoice): +0 Fill, +4 Update, +8
 * GateOn, +12 GateOff, +16 Instantiate, +20 Free, +24 Install (branches),
 * +28 offset to the name. Entries are called with the return address
 * pushed on the stack (they exit with LDMFD R13!,{PC}) and R6 >= 0,
 * R7 = channel - 1, R9 = the channel's SCCB, R10 = buffer end,
 * R11 = channel count (sample stride), R12 = the channel's first sample;
 * Fill/Update/GateOn return R0 = 8 while the channel stays active, GateOff
 * returns the channel's final state (the buffer flushed); Instantiate and
 * Free are called (in SVC mode, from the SWI) when a voice is attached to
 * or detached from a channel, Install when it is installed. SCCB layout:
 * +0 amplitude, +1 voice, +3 flags, +4 pitch (the 16-bit increment added
 * to the top of a 32-bit phase accumulator per sample; 256-byte cycles),
 * +12 duration as a number of buffer fills; the rest is the voice's. As in
 * RISC OS, the channel handler doesn't count durations down: a voice that
 * honours them decrements +12 itself and stops (sample voices commonly
 * ignore it and play the whole sample). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define DEFAULT_LEVEL1 0x017F4800u        /* the OS's own Level 1 handler block */
#define LOG_SCALE      0x017F4880u        /* its log amplitude scale table */
#define LIN_TO_LOG     0x017F4A00u        /* its linear-to-log table */
static uint32_t snd_nchan = 1, snd_samples = 208, snd_period = 48, snd_level1 = DEFAULT_LEVEL1, snd_level2,
                snd_enabled = 2;
static int snd_pan[8];
static uint64_t snd_acc_us;
static int snd_buf;
static int16_t snd_linear[128];

#define SCCB_ACTIVE  0x08
static uint32_t snd_voice[33];            /* installed voice generators, by slot */
static uint8_t snd_attached[8];           /* voice slot per channel */
static int snd_pend[8];                   /* pending voice calls, as SoundChannels' SCCB flags:
                                             &80 gate off, &40 gate on, &20 update */
#define DEFAULT_TUNING 0x6AB0                /* the master pitch: &4000 is middle C */
static uint32_t snd_tuning = DEFAULT_TUNING;

static void control(int chan, int32_t amp, uint32_t pitch, uint32_t dur);
static void attach(int c, uint32_t slot);

static void voice_call(int c, uint32_t entry, uint32_t buf, uint32_t len)
{
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
    memcpy(saved, R, sizeof R);
    bank_r13[2] = IRQ_STACK - 4;
    WR32(IRQ_STACK - 4, HANDLER_RET | MODE_IRQ);
    R[6] = 0; R[7] = (uint32_t)c; R[9] = SCCB_AREA + c * 0x100;
    R[10] = buf + len; R[11] = snd_nchan; R[12] = buf + c;
    call_code(entry, MODE_IRQ, 0);
    uint32_t state = R[0] & 0xFF;
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    MEM[SCCB_AREA + c * 0x100 + 3] = (uint8_t)state;
}

/* Install / Instantiate / Free: from a SWI, in SVC mode on the SVC stack */
static void voice_admin(int c, uint32_t entry)
{
    uint32_t saved[16], sf[4] = {NF, ZF, CF, VF}, sr13 = bank_r13[3];
    memcpy(saved, R, sizeof R);
    int svc = (MODEBITS & 3) == 3;                   /* e.g. a SWI from module code */
    uint32_t sp = (svc ? R[13] : SVC_STACK) - 4;
    WR32(sp, HANDLER_RET | MODE_SVC);                /* the entry returns with LDMFD R13!,{PC} */
    if (svc) R[13] = sp; else bank_r13[3] = sp;
    R[0] = (uint32_t)c + 1; R[7] = (uint32_t)c; R[9] = SCCB_AREA + c * 0x100;
    call_code(entry, MODE_SVC, 0);
    memcpy(R, saved, sizeof R);
    NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    bank_r13[3] = sr13;
}

/* attach voice slot `slot` to channel c (0-based): Free the old, Instantiate the new */
static void attach(int c, uint32_t slot)
{
    uint32_t old = snd_voice[snd_attached[c]];
    if (old && old != BEEP_VOICE && snd_attached[c] != slot) voice_admin(c, old + 20);
    snd_attached[c] = (uint8_t)slot;
    if (snd_voice[slot] && snd_voice[slot] != BEEP_VOICE) voice_admin(c, snd_voice[slot] + 16);
}

/* WaveSynth-Beep, the ROM's voice 1 and every channel's voice to begin
 * with, made here: a sine at the channel's pitch and amplitude, a short
 * attack, the duration counted in buffer fills, a short release. Its
 * header (BEEP_VOICE) is only there for its name. */
static struct { uint32_t phase; float env; int gate; } beep[8];
static float beep_sine[256];

static void beep_install(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int k = 0; k < 256; k++) beep_sine[k] = (float)sin(k * 2 * M_PI / 256);
    if (!snd_voice[1]) {
        memset(MEM + BEEP_VOICE, 0, 64);
        WR32(BEEP_VOICE + 28, 32);
        strcpy((char *)MEM + BEEP_VOICE + 32, "WaveSynth-Beep");
        snd_voice[1] = BEEP_VOICE;
    }
    for (int c = 0; c < 8; c++) if (!snd_attached[c]) snd_attached[c] = 1;
}

static void beep_fill(int c, uint32_t buf, uint32_t len)
{
    uint32_t sccb = SCCB_AREA + c * 0x100;
    if (snd_pend[c] & 0x80) beep[c].gate = 0;                       /* gate off */
    if (snd_pend[c] & 0x40) { beep[c].gate = 1; beep[c].phase = 0; beep[c].env = 0; }   /* gate on */
    snd_pend[c] = 0;
    if (beep[c].gate) {                                            /* the duration, in fills */
        uint32_t d = RD32(sccb + 12);
        if (d == 0) beep[c].gate = 0;
        else if (d != 0x7FFFFFFF) WR32(sccb + 12, d - 1);
    }
    if (!beep[c].gate && beep[c].env <= 0) {
        MEM[sccb + 3] = 0;
        for (uint32_t k = c; k < len; k += snd_nchan) MEM[buf + k] = 0;
        return;
    }
    MEM[sccb + 3] = SCCB_ACTIVE;
    float amp = powf(2.0f, ((MEM[sccb] & 0x7F) - 127) / 16.0f) * 4095.0f;
    uint32_t inc = RD32(sccb + 4) << 16;
    float attack = 1.0f / (2000.0f / snd_period), release = 1.0f / (20000.0f / snd_period);
    for (uint32_t k = c; k < len; k += snd_nchan) {
        if (beep[c].gate) { beep[c].env += attack; if (beep[c].env > 1) beep[c].env = 1; }
        else { beep[c].env -= release; if (beep[c].env < 0) beep[c].env = 0; }
        int v = (int)lroundf(beep_sine[beep[c].phase >> 24] * amp * beep[c].env);
        beep[c].phase += inc;
        MEM[buf + k] = MEM[LIN_TO_LOG + (v & 0x1FFF)];
    }
}

static void tables_init(void)
{
    WR32(DEFAULT_LEVEL1, 0); WR32(DEFAULT_LEVEL1 + 4, 0);
    WR32(DEFAULT_LEVEL1 + 8, LIN_TO_LOG); WR32(DEFAULT_LEVEL1 + 12, LOG_SCALE);
    for (int k = 0; k < 256; k++) MEM[LOG_SCALE + k] = (uint8_t)k;
    for (int k = 0; k < 8192; k++) {               /* nearest VIDC log value */
        int v = k < 4096 ? k : k - 8192, a = v < 0 ? -v : v, best = 0;
        for (int m = 0; m < 128; m++)
            if (abs(((2 * (m & 15) + 33) << (m >> 4)) - 33 - 2 * a) < abs(((2 * (best & 15) + 33) << (best >> 4)) - 33 - 2 * a))
                best = m;
        MEM[LIN_TO_LOG + k] = (uint8_t)(best << 1 | (v < 0));
    }
}

static void channel_fill(uint32_t buf, uint32_t len)
{
    beep_install();
    if (!MEM[LOG_SCALE + 255]) tables_init();
    for (uint32_t c = 0; c < snd_nchan; c++) {
        uint32_t v = snd_voice[snd_attached[c]], sccb = SCCB_AREA + c * 0x100;
        if (v == BEEP_VOICE) { beep_fill((int)c, buf, len); continue; }
        /* one voice call a fill: gate off, then gate on (a new note at
         * amplitude 0 is both, on two fills), then update, then fill */
        if (v && (snd_pend[c] & 0x80)) { snd_pend[c] &= ~0x80; voice_call(c, v + 12, buf, len); continue; }
        if (v && (snd_pend[c] & 0x40)) { snd_pend[c] &= ~0x40; voice_call(c, v + 8, buf, len); continue; }
        if (v && (snd_pend[c] & 0x20)) { snd_pend[c] = 0; voice_call(c, v + 4, buf, len); continue; }
        if (v && (MEM[sccb + 3] & SCCB_ACTIVE)) { voice_call(c, v, buf, len); continue; }
        for (uint32_t k = c; k < len; k += snd_nchan) MEM[buf + k] = 0;
    }
}

/* The scheduler (Sound_QSchedule): calls to make after a number of beats,
 * at Sound_QTempo's tempo (&1000: a beat a centisecond) - a scheduled
 * Sound_ControlPacked (R1 0), or another sound SWI given by number, with
 * R2/R3 as its R0/R1 */
#define QMAX 64
static struct { uint64_t at; uint32_t swi, r0, r1; } q[QMAX];
static int nq;
static uint32_t q_tempo = 0x1000;
static uint64_t q_now;                    /* centiseconds */
static int sound_impl(uint32_t b);

static void q_call(uint32_t swi, uint32_t r0, uint32_t r1)
{
    uint32_t saved[4] = {R[0], R[1], R[2], R[3]};
    R[0] = r0; R[1] = r1;
    if (!swi) control((int)(r0 & 0xFFFF), (int16_t)(r0 >> 16), r1 & 0xFFFF, r1 >> 16);
    else { roger_log_sound_swi(swi); sound_impl(swi); }
    memcpy(R, saved, sizeof saved);
}

/* every centisecond (durations are the voices' business: see above) */
void sound_tick(void)
{
    q_now++;
    for (int k = 0; k < nq; )
        if (q[k].at <= q_now) {
            uint32_t s = q[k].swi, a = q[k].r0, b = q[k].r1;
            q[k] = q[--nq];
            q_call(s, a, b);
        } else k++;
}

/* *ChannelVoice: attach a voice (a slot number or a name) to a channel */
int sound_channel_voice(int chan, const char *voice)
{
    beep_install();
    if (chan < 1 || chan > 8) return 0;
    char *e;
    long slot = strtol(voice, &e, 10);
    if (*voice && !*e) {
        if (slot < 1 || slot > 32 || !snd_voice[slot]) return 0;
        attach(chan - 1, (uint32_t)slot);
        return 1;
    }
    for (int s = 1; s <= 32; s++) {
        if (!snd_voice[s]) continue;
        uint32_t name = snd_voice[s] + RD32(snd_voice[s] + 28);
        char have[32];
        int k;
        for (k = 0; k < 31 && MEM[name + k] > 32; k++) have[k] = MEM[name + k];
        have[k] = 0;
        if (!strcasecmp(have, voice)) { attach(chan - 1, (uint32_t)s); return 1; }
    }
    return 0;
}

/* VIDC log bytes (snd_nchan interleaved channels) at buf to the host:
 * 16-bit stereo */
static void sound_out(uint32_t buf, uint32_t len)
{
    if (!snd_linear[127])
        for (int m = 0; m < 128; m++)                       /* VIDC log: 3-bit chord, 4-bit step */
            snd_linear[m] = (int16_t)((((2 * (m & 15) + 33) << (m >> 4)) - 33) * 4);
    static int16_t out[2 * 2048];
    uint32_t frames = len / snd_nchan;
    if (frames > 2048) frames = 2048;
    for (uint32_t f = 0; f < frames; f++) {
        int l = 0, r = 0;
        for (uint32_t c = 0; c < snd_nchan; c++) {
            uint8_t b = MEM[buf + f * snd_nchan + c];
            int v = snd_linear[b >> 1];
            if (b & 1) v = -v;
            int pan = snd_pan[c];
            l += v * (pan <= 0 ? 127 : 127 - pan) / 127;
            r += v * (pan >= 0 ? 127 : 127 + pan) / 127;
        }
        l /= (int)snd_nchan; r /= (int)snd_nchan;   /* VIDC time-multiplexes channels: each gets 1/nchan */
        out[2 * f] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * f + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    roger_audio_out(out, (int)frames, (int)(1000000 / snd_period));
}

static void sound_dma_fill(void)
{
    uint32_t len = snd_nchan * snd_samples, buf = DMA_BUF + (snd_buf ^= 1) * 0x2000;
    if (len > 0x2000) len = 0x2000;
    if (snd_level1 != DEFAULT_LEVEL1 && snd_enabled == 2) {
        uint32_t saved[16], sf[4] = {NF, ZF, CF, VF};
        memcpy(saved, R, sizeof R);
        R[9] = snd_level1; R[10] = buf + len; R[11] = snd_nchan; R[12] = buf;
        bank_r13[2] = IRQ_STACK;               /* each interrupt starts on an empty IRQ stack */
        call_code(RD32(snd_level1), MODE_IRQ, HANDLER_RET | MODE_IRQ);
        memcpy(R, saved, sizeof R);
        NF = sf[0]; ZF = sf[1]; CF = sf[2]; VF = sf[3];
    } else if (snd_enabled == 2) channel_fill(buf, len);
    else memset(MEM + buf, 0, len);
    sound_out(buf, len);
}

/* ---------------------------------------------------------------- MEMC sound DMA
 * A program that claims the sound DMA interrupt (device 9) drives the DMA
 * itself, as RISC OS's SoundDMA module would: MEMC plays the bytes of the
 * current buffer, then reloads its pointers from Sstart and SendN (the
 * next buffer, which the interrupt handler programmed) and interrupts; the
 * handler programs the one after. Physical addresses are screen memory
 * from 0 (as for video DMA) and the page at &78000 holding the OS's sound
 * buffers, which RISC OS 3.1 maps at &1F00000 (its two buffers are
 * &1F06000 and &1F07000, physical &7E000 and &7F000) - where MEMC points
 * until the program changes it. Without such a claimant, libroger's own
 * SoundDMA above fills its buffers through the Level 1 handler. */
#define OS_SNDBUF(k) (0x7E000u + 0x1000u * (k))
static uint32_t memc_sstart = OS_SNDBUF(0), memc_sendn = OS_SNDBUF(0) + 208 - 16;
static uint32_t cur_start = OS_SNDBUF(1), cur_end = OS_SNDBUF(1) + 208 - 16;

void sound_memc(int reg, uint32_t v)
{
    if (reg == 4) memc_sstart = v;
    else if (reg == 5) memc_sendn = v;
    else { cur_start = memc_sstart; cur_end = memc_sendn; }   /* Sptr: start again from Sstart */
}

static uint32_t sound_phys(uint32_t p)
{
    if (p >= 0x78000 && p < 0x80000) return 0x1F00000 + (p - 0x78000);
    if (p < screen_size()) return screen_base() + p;
    return 0;
}

static uint32_t level0_len(void)              /* the current buffer's length (SendC is its last 16 bytes) */
{
    return cur_end >= cur_start && cur_end - cur_start < 0x10000 ? cur_end + 16 - cur_start : 0;
}

static void sound_level0(void)
{
    uint32_t len = level0_len(), a = sound_phys(cur_start);
    if (len && a && snd_enabled == 2) sound_out(a, len);
    cur_start = memc_sstart; cur_end = memc_sendn;
    irq_sound();
}

/* Advance the sound DMA clock by `us` of emulated time. */
void sound_dma_advance(uint32_t us)
{
    for (;;) {
        int level0 = irq_sound_claimed();
        uint64_t buf_us = level0 ? (uint64_t)(level0_len() / snd_nchan) * snd_period
                                 : (uint64_t)snd_samples * snd_period;
        if (!buf_us) buf_us = (uint64_t)snd_samples * snd_period;
        if (!buf_us) return;
        if (us) { snd_acc_us += us; us = 0; }
        if (snd_acc_us < buf_us) return;
        snd_acc_us -= buf_us;
        if (level0) sound_level0(); else sound_dma_fill();
    }
}

void roger_log_sound_swi(uint32_t b)
{
    int nargs = 3;
    switch (b) {
    case 0x40140: nargs = 5; break; case 0x40141: case 0x40143: case 0x40180:
    case 0x40184: nargs = 1; break; case 0x40142: case 0x40183: case 0x40185:
    case 0x4018A: nargs = 2; break; case 0x40189: nargs = 4; break;
    }
    char name[32]; sprintf(name, "Sound_%x", b);
    const char *fmts[] = {"", "i", "ii", "iii", "iiii", "iiiii"};
    ev(name, fmts[nargs], R[0], R[1], R[2], R[3], R[4]);
}

/* Sound_Pitch: 15-bit pitch (&4000 = middle C at the default tuning, 4096
 * per octave; the tuning, *Tuning / Sound_Tuning, is added) to the phase
 * accumulator step for a 256-sample wavetable. The OS's pitch table is for
 * the default 48us sample period: at other periods the same steps are
 * taken per (slower or faster) sample (e.g. Galactic Dan's 96us: its
 * notes' durations match their samples' lengths only that way). */
static uint32_t pitch_step(uint32_t p)
{
    double hz = 261.6256 * pow(2.0, ((double)p + (int)snd_tuning - DEFAULT_TUNING - 0x4000) / 4096.0);
    return (uint32_t)(hz * 256.0 * 48e-6 * 4294967296.0 / 256.0);
}

/* Sound_Control, as SoundChannels 1.27 does it. Amplitude: &100-&1FF
 * logarithmic (bit 7 set: change the sound already playing, no new note),
 * or -15..0 as BBC BASIC's SOUND (log &7F down to &43: 0 isn't silent);
 * anything else changes nothing. A new note (bit 7 clear) gates the voice
 * on; at log amplitude 0 it's gated off first; bit 7 set calls the voice's
 * update entry instead. Pitch: &8000 and up the
 * phase increment itself, &100-&7FFF (&4000 middle C, 4096 per octave) or
 * 0-255 as BBC BASIC's (53 middle C, 48 per octave). Duration in 5cs units,
 * &FF forever, stored in the SCCB as buffer fills; 0 leaves the duration
 * as it was (a game re-pitching a running engine note every frame:
 * Stunt Racer 2000). */
static void control(int chan, int32_t amp, uint32_t pitch, uint32_t dur)
{
    if (chan < 1 || chan > 8) return;
    int c = chan - 1;
    uint32_t sccb = SCCB_AREA + c * 0x100;
    int logamp;
    if (amp >= 0x100 && amp <= 0x1FF) logamp = amp & 0xFF;
    else if (amp <= 0 && amp >= -15) logamp = 0x7F ^ (((amp - 1) & 15) << 2);
    else return;
    MEM[sccb] = (uint8_t)(logamp & 0x7F);
    MEM[sccb + 1] = snd_attached[c];
    if (pitch >= 0x8000) WR32(sccb + 4, pitch & 0xFFFF);
    else {
        uint32_t p = pitch >= 0x100 ? pitch : (uint32_t)(0x4000 + ((int)pitch - 53) * 4096 / 48);
        WR32(sccb + 4, pitch_step(p) >> 16);       /* the 16-bit phase increment */
    }
    if (dur == 0xFF) WR32(sccb + 12, 0x7FFFFFFF);
    else if (dur) {
        uint64_t fills = (uint64_t)dur * 50000 / ((uint64_t)snd_samples * snd_period);
        WR32(sccb + 12, fills > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)fills);
    }
    /* (replacing any call still pending: the voice applies the amplitude) */
    snd_pend[c] = ((logamp & 0x7F) == 0 ? 0x80 : 0) | (logamp & 0x80 ? 0x20 : 0x40);
}

static int sound_impl(uint32_t b)
{
    switch (b) {
    case 0x40140: {                          /* Sound_Configure */
        if (!MEM[LOG_SCALE + 255]) tables_init();
        uint32_t old[5] = {snd_nchan, snd_samples, snd_period, snd_level1, snd_level2};
        if (R[0]) { uint32_t n = 1; while (n < R[0] && n < 8) n <<= 1; snd_nchan = n; }
        if (R[1]) snd_samples = R[1];
        if (R[2]) snd_period = R[2];
        if (R[3]) snd_level1 = R[3];
        if (R[4]) snd_level2 = R[4];
        for (int k = 0; k < 5; k++) R[k] = old[k];
        return 1;
    }
    case 0x40141: {                          /* Sound_Enable: 0 read, 1 off, 2 on */
        uint32_t old = snd_enabled;
        if (R[0] == 1 || R[0] == 2) snd_enabled = R[0];
        R[0] = old;
        return 1;
    }
    case 0x40142: {                          /* Sound_Stereo: R0 channel 1-8, R1 position */
        int c = (int)(R[0] - 1) & 7, old = snd_pan[c];
        int32_t p = (int32_t)R[1];
        if (p != -128) snd_pan[c] = p < -127 ? -127 : p > 127 ? 127 : p;
        R[1] = (uint32_t)old;
        return 1;
    }
    case 0x40143: R[0] = 2; return 1;      /* Sound_Speaker */
    case 0x40180: R[0] = 127; return 1;    /* Sound_Volume */
    case 0x40181:                          /* Sound_SoundLog: 32-bit signed linear -> VIDC log
                                              (top 13 bits through the linear-to-log table; e.g.
                                              sample voices converting their samples at init) */
        if (!MEM[LOG_SCALE + 255]) tables_init();
        R[0] = MEM[LIN_TO_LOG + ((R[0] >> 19) & 0x1FFF)];
        return 1;
    case 0x40182:                          /* Sound_LogScale: log amplitude scaled by the volume */
        if (!MEM[LOG_SCALE + 255]) tables_init();
        R[0] = (R[0] & ~0xFEu) | MEM[LOG_SCALE + (R[0] & 0xFE)];
        return 1;
    case 0x401C0: nq = 0; return 1;        /* Sound_QInit */
    case 0x401C1: {                        /* Sound_QSchedule: R0 beats from now, R1 0 or a SWI, R2/R3 */
        uint32_t swi = R[1] & 0x20000 ? R[1] & ~0x20000u : R[1];
        if (swi && (swi & ~0x3Fu) != 0x40140 && (swi & ~0x3Fu) != 0x40180) { R[0] = (uint32_t)-1; return 1; }
        int32_t beats = (int32_t)R[0];
        if (beats <= 0) { q_call(swi, R[2], R[3]); R[0] = 0; return 1; }
        if (nq == QMAX) { R[0] = (uint32_t)-1; return 1; }
        q[nq].at = q_now + (uint64_t)beats * 0x1000 / (q_tempo ? q_tempo : 0x1000);
        q[nq].swi = swi; q[nq].r0 = R[2]; q[nq].r1 = R[3];
        nq++;
        R[0] = 0;
        return 1;
    }
    case 0x401C2: nq = 0; return 1;        /* Sound_QRemove (all, here) */
    case 0x401C3: R[0] = (uint32_t)(QMAX - nq); return 1;   /* Sound_QFree */
    case 0x401C5: {                        /* Sound_QTempo: R0 0 read, else the new tempo */
        uint32_t old = q_tempo;
        if (R[0]) q_tempo = R[0];
        R[0] = old;
        return 1;
    }
    case 0x401C6: R[0] = 0; return 1;      /* Sound_QBeat: no bar counting */
    case 0x40183: {                        /* Sound_InstallVoice: R0 header, R1 slot (0: any free) */
        uint32_t slot = R[1];
        if (!slot) for (slot = 1; slot < 32 && snd_voice[slot]; slot++) ;
        if (slot < 1 || slot > 32) { R[1] = 0; return 1; }
        snd_voice[slot] = R[0];
        R[1] = slot;
        uint32_t header = R[0];
        R[0] = header + RD32(header + 28); /* the voice's name */
        voice_admin(0, header + 24);      /* Install */
        return 1;
    }
    case 0x40184:                          /* Sound_RemoveVoice */
        if (R[1] >= 1 && R[1] <= 32) snd_voice[R[1]] = 0;
        return 1;
    case 0x40185: {                        /* Sound_AttachVoice: R0 channel 1-8, R1 slot (0: read) */
        int c = (int)(R[0] - 1) & 7;
        uint32_t old = snd_attached[c];
        if (R[1] && R[1] <= 32) attach(c, R[1]);
        R[1] = old;
        return 1;
    }
    case 0x40187: {                        /* Sound_Tuning: R0 0 read, else the new master pitch */
        uint32_t old = snd_tuning;
        if (R[0] & 0xFFFF) snd_tuning = R[0] & 0xFFFF;
        R[0] = old;
        return 1;
    }
    case 0x40188:                          /* Sound_Pitch */
        R[0] = pitch_step(R[0] & 0x7FFF);
        return 1;
    case 0x40189:                          /* Sound_Control: R0 channel, R1 amplitude, R2 pitch, R3 duration */
        control((int)R[0], (int32_t)R[1], R[2], R[3]);
        return 1;
    case 0x40186:                          /* Sound_ControlPacked: R0 amplitude << 16 | channel,
                                              R1 duration << 16 | pitch */
        control((int)(R[0] & 0xFFFF), (int16_t)(R[0] >> 16), R[1] & 0xFFFF, R[1] >> 16);
        return 1;
    case 0x4018A: {                        /* Sound_AttachNamedVoice: R0 channel, R1 name */
        char want[16], have[16];
        int k;
        for (k = 0; k < 15 && MEM[R[1] + k] > 32; k++) want[k] = MEM[R[1] + k];
        want[k] = 0;
        for (int slot = 1; slot <= 32; slot++) {
            if (!snd_voice[slot]) continue;
            uint32_t name = snd_voice[slot] + RD32(snd_voice[slot] + 28);
            for (k = 0; k < 15 && MEM[name + k] > 32; k++) have[k] = MEM[name + k];
            have[k] = 0;
            if (!strcasecmp(have, want)) { attach((R[0] - 1) & 7, slot); return 1; }
        }
        R[0] = 0;                          /* no such voice */
        return 1;
    }
    }
    return 0;
}

/* The Sound SWI chunks (&40140, &40180, &401C0): 1 if b is one */
int sound_swi(uint32_t b)
{
    uint32_t grp = b & ~0x3Fu;
    if (grp != 0x40140 && grp != 0x40180 && grp != 0x401C0) return 0;
    beep_install();
    roger_log_sound_swi(b);
    sound_impl(b);
    return 1;
}
