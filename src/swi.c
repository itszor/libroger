/* SWI dispatch and the kernel's own SWIs: VDU output, OS_Byte, OS_Word,
 * the environment, conversions. Files, sound, vectors and modules have
 * their own files. Every call worth knowing about is written to the event
 * log (roger_log()). */
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include "internal.h"

uint32_t swi_pc;
uint32_t env[17][3];                             /* OS_ChangeEnvironment's handlers: code, R12, buffer */

/* INKEY -(inkey): host keyboard (by internal key number + 1), or in
 * headless runs the scripted keys */
int pressed(int inkey)
{
    /* the mouse buttons are keys too (INKEY -10 Select, -11 Menu, -12
     * Adjust; their transitions low-level keys &70-&72) */
    if (inkey >= 10 && inkey <= 12 && (mouse_buttons & (4 >> (inkey - 10)))) return 1;
    if (inkey >= 1 && inkey <= 128 && key_held(inkey - 1)) return 1;   /* (headless: --script keys) */
    if (rg_interactive) return 0;
    for (unsigned k = 0; k < scenario_keys(); k++)
        if (rg->keys[k].inkey == inkey && rg->keys[k].from <= icount && icount < rg->keys[k].to) return 1;
    return 0;
}

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void os_byte(void)
{
    uint32_t op = R[0] & 0xFF;
    if (buffers_os_byte(op)) return;
    if (op == 134 || op == 165) {                      /* the text cursor (134), the output cursor */
        int x, y;                                      /* (165, the same here): R1 x, R2 y, in the */
        vdu_text_cursor(&x, &y);                       /* text window (BASIC's POS, VPOS) */
        R[1] = (uint32_t)x; R[2] = (uint32_t)y;
        return;
    }
    if (op == 71) {                                    /* the alphabet (R1 < 128) or keyboard: */
        R[1] = R[1] & 0x80 ? 1 : 101;                  /* the UK's and Latin1, read or the old one */
        return;                                        /* (Star Fighter 3000's Keyboards.1 names) */
    }
    if (op == 129) {                                   /* INKEY */
        if (R[2] == 0xFF && (R[1] & 0xFF) == 0) {      /* the OS version: 3.1, or 3.5 on a Risc PC */
            R[1] = rg->vidc20 ? 0xA5 : 0xA4; R[2] = 0;
        } else if (R[2] == 0xFF && (R[1] & 0xFF) >= 0x80) {   /* scan one key (R1's low byte) */
            int k = 256 - (int)(R[1] & 0xFF);
            R[1] = R[2] = pressed(k) ? 0xFF : 0;
        } else if (rg_interactive) {                    /* read a key, with time limit: R1 the
                                                           character, or as R2 (&1B Escape, &FF
                                                           timeout), as RISC OS gives it (Tertis'
                                                           level entry tests R1 for &FF) */
            uint64_t until = display_ticks_ns() + (uint64_t)((R[2] & 0x7F) << 8 | R[1]) * CS_NS;
            for (;;) {
                if (escape_pressed) { R[1] = R[2] = 27; return; }
                int c = key_buffer_get();
                if (c >= 0) { R[1] = c; R[2] = 0; return; }
                if (display_ticks_ns() >= until) { R[1] = R[2] = 0xFF; return; }
                display_sleep_ns(CS_NS);
                rt_service(0);
            }
        } else {
            int c = key_buffer_get();                   /* (--script's type:) */
            if (c >= 0) { R[1] = c; R[2] = 0; return; }
            R[1] = R[2] = 0xFF;
            for (unsigned k = 0; k < scenario_keys(); k++)   /* a scripted Space reads as ' ' */
                if (rg->keys[k].from <= icount && icount < rg->keys[k].to && rg->keys[k].inkey == 99) {
                    R[1] = 32; R[2] = 0;
                }
        }
        return;
    }
    if (op == 121 || op == 122) {                      /* keyboard scan: R1 key ^ &80 (that key), */
        if (op == 121 && (R[1] & 0x80)) { R[1] = pressed((R[1] ^ 0x80) + 1) ? 0xFF : 0; return; }
        uint32_t k = op == 122 ? 16 : R[1];            /* or the first key from R1 (122: 16) up pressed */
        while (k < 128 && !pressed(k + 1)) k++;
        R[1] = k < 128 ? k : 0xFF;
        return;
    }
    if (op == 202) {                                   /* keyboard status: (old AND R2) EOR R1 */
        uint8_t old = (kbd_status & ~0x48) | (key_held(0) ? 0x08 : 0) | (key_held(1) ? 0x40 : 0);
        kbd_status = ((old & R[2]) ^ R[1]) & ~0x48;
        R[1] = old;
        return;
    }
    if (op == 112 || op == 113) {                      /* screen bank for VDU drivers / display: */
        uint32_t old = (op == 113 ? display_bank : bank) + 1;   /* R1 the new one (1-based), returns */
        video_set_bank(op == 113, (R[1] >= 1 ? R[1] - 1 : 0) & 1);   /* the previous one */
        ev("OS_Byte", "ii", op, R[1]);
        R[1] = old;
        return;
    }
    if (op == 13 || op == 14) {                        /* disable / enable event */
        uint32_t m = 1u << (R[1] & 31);
        ev("OS_Byte", "iii", op, R[1], R[2]);
        R[1] = (events_enabled & m) ? 1 : 0;              /* old state */
        if (op == 14) events_enabled |= m; else events_enabled &= ~m;
        return;
    }
    if (op == 176) {                                   /* the vsync counter: read/write */
        extern uint8_t vsync_counter;
        uint8_t old = vsync_counter;
        vsync_counter = (uint8_t)((old & R[2]) ^ R[1]);
        R[1] = old;
        return;
    }
    if (op == 19) { if (rg_interactive) wait_vsync(); else monotonic++; return; }
    if (rg_interactive && op >= 124 && op <= 126) {    /* clear / set / acknowledge Escape */
        if (op == 126 && escape_pressed) key_buffer_flush();   /* escape side effect */
        if (op != 125) { if (escape_pressed) ev("escape_cleared", "ii", op, swi_pc - 4); escape_pressed = 0; }
        else escape_pressed = 1;
        R[1] = op == 126 ? 0xFF : R[1];
        return;
    }
    if (op == 135) { R[1] = 0; R[2] = screen_mode; return; }
    if (op == 106) pointer_shape = R[1] & 0x7F;      /* select pointer (bit 7: unlinked from the mouse) */   /* character at cursor, screen mode */
    /* OS_Byte 15 (flush all / current input buffer), 21 (flush buffer r1; 0 = keyboard) */
    if (rg_interactive && (op == 15 || (op == 21 && R[1] == 0))) key_buffer_flush();
    ev("OS_Byte", "iii", op, R[1], R[2]);
}

/* The real-time clock: centiseconds since 1900. Interactive runs read the
 * host's clock; headless runs start at a fixed moment (noon, 1 January
 * 1993) and advance with emulated time, so that they stay deterministic. */
static uint64_t rtc_cs(void)
{
    if (rg_interactive) return ((uint64_t)time(NULL) + 2208988800ull) * 100;   /* 1900 -> 1970 */
    return 2934878400ull * 100 + icount / TICK_STEPS;
}

uint8_t kbd_status = 0x30;                   /* Caps Lock and Shift Lock off */

/* The system clock (BASIC's TIME): centiseconds, 5 bytes, counting with
 * the monotonic timer from the value OS_Word 2 last wrote (0 at start) */
static uint64_t sysclock_base;

static void os_word(void)
{
    uint8_t *blk = MEM + R[1];
    if (R[0] == 1 || R[0] == 2) {                    /* read / write the system clock */
        if (R[0] == 1) {
            uint64_t t = (monotonic - sysclock_base) & 0xFFFFFFFFFFull;
            for (int k = 0; k < 5; k++) blk[k] = (uint8_t)(t >> 8 * k);
        } else {
            uint64_t t = 0;
            for (int k = 4; k >= 0; k--) t = t << 8 | blk[k];
            sysclock_base = monotonic - t;
        }
        ev("OS_Word", "is", R[0], blk, 5);
        return;
    }
    if (R[0] == 14 && blk[0] == 3) {                 /* read the RTC: 5 bytes, centiseconds */
        uint64_t t = rtc_cs();
        for (int k = 0; k < 5; k++) blk[k] = (uint8_t)(t >> 8 * k);
        return;
    }
    /* read a character's definition: Elite keeps the character set this
     * way while its game redefines it, and puts it back with VDU 23 */
    if (R[0] == 10 && blk[0] >= 32) vdu_char_def(blk[0], blk + 1);
    video_os_word(R[0], blk);
    if (R[0] == 21 && blk[0] == 0)
        ev("OS_Word", "isi", R[0], blk, 6, blk[6] | blk[7] << 8 | blk[8] << 16 | (uint32_t)blk[9] << 24);
    else ev("OS_Word", "is", R[0], blk, 8);
}

/* OS_ConvertHex1..8, OS_ConvertCardinal1..4, OS_ConvertSpacedCardinal1..4 */
static void convert(uint32_t b)
{
    uint32_t v = R[0], buf = R[1], size = R[2];
    char s[64];
    if (b <= 0xD4) {
        int digits = b == 0xD0 ? 1 : b == 0xD1 ? 2 : b == 0xD2 ? 4 : b == 0xD3 ? 6 : 8;
        uint64_t m = (digits == 8) ? 0xFFFFFFFFull : ((1ull << (4 * digits)) - 1);
        sprintf(s, "%0*llX", digits, (unsigned long long)(v & m));
    } else {
        int k = (b - 0xD5) % 4;
        if (k < 3) v &= (1u << (8 * (k + 1))) - 1;
        if (b >= 0xE1) {                         /* spaced: groups of three */
            char t[32]; int n = sprintf(t, "%u", v), o = 0;
            for (int i = 0; i < n; i++) {
                s[o++] = t[i];
                if ((n - 1 - i) % 3 == 0 && i != n - 1) s[o++] = ' ';
            }
            s[o] = 0;
        } else sprintf(s, "%u", v);
    }
    int l = strlen(s);
    memcpy(MEM + buf, s, l + 1);
    R[0] = buf; R[1] = buf + l; R[2] = size - l - 1;
}

/* OS_BinaryToDecimal: R0 a signed integer, as decimal digits (a leading
 * "-" if negative, no terminator) into R1's buffer of R2 bytes; R2 returns
 * the length. Buffer overflow if it doesn't fit. */
static void binary_to_decimal(void)
{
    char s[16];
    int l = sprintf(s, "%d", (int32_t)R[0]);
    if ((uint32_t)l > R[2]) { os_error(0x1E4, "Buffer overflow"); return; }
    memcpy(MEM + R[1], s, l);
    R[2] = l;
}

/* OS_ConvertDateAndTime (R3 the format) and OS_ConvertStandardDateAndTime
 * (Sys$DateFormat's default): R0 a 5-byte time, centiseconds since 1900
 * (UTC), R1/R2 the buffer and its size. The format's %-codes: CS SE MI 12
 * 24 AM PM WE W3 WN DY ST MO M3 MN CE YR WK DN %; %Z before one drops its
 * leading zero. */
static void convert_time(uint32_t b)
{
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"};
    uint64_t cs = 0;
    for (int k = 4; k >= 0; k--) cs = cs << 8 | MEM[R[0] + k];
    time_t t = (time_t)(cs / 100) - 2208988800LL;     /* 1900 -> 1970 */
    struct tm tm;
    gmtime_r(&t, &tm);
    char fmt[256], out[256];
    if (b == 0xC1) cstr(R[3], fmt, 1); else strcpy(fmt, "%W3,%DY %M3 %CE%YR.%24:%MI:%SE");
    int o = 0;
    for (const char *p = fmt; *p && o < 200; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        int z = 0;
        if ((p[1] | 32) == 'z') { z = 1; p++; }
        char c1 = p[1] ? (p[1] & ~32) : 0, c2 = c1 && p[2] ? (p[2] & ~32) : 0, v[32] = "";
        int num = -1, two = 1;
        if (p[1] == '%') { out[o++] = '%'; p++; continue; }
        if (c1 == 'C' && c2 == 'S') num = (int)(cs % 100);
        else if (c1 == 'S' && c2 == 'E') num = tm.tm_sec;
        else if (c1 == 'M' && c2 == 'I') num = tm.tm_min;
        else if (c1 == '1' && c2 == '2') num = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        else if (c1 == '2' && c2 == '4') num = tm.tm_hour;
        else if ((c1 == 'A' || c1 == 'P') && c2 == 'M') strcpy(v, tm.tm_hour < 12 ? "AM" : "PM");
        else if (c1 == 'W' && c2 == 'E') strcpy(v, days[tm.tm_wday]);
        else if (c1 == 'W' && c2 == '3') snprintf(v, 4, "%s", days[tm.tm_wday]);
        else if (c1 == 'W' && c2 == 'N') { num = tm.tm_wday + 1; two = 0; }
        else if (c1 == 'D' && c2 == 'Y') num = tm.tm_mday;
        else if (c1 == 'S' && c2 == 'T') { int d = tm.tm_mday;
            strcpy(v, d % 10 == 1 && d != 11 ? "st" : d % 10 == 2 && d != 12 ? "nd" : d % 10 == 3 && d != 13 ? "rd" : "th"); }
        else if (c1 == 'M' && c2 == 'O') strcpy(v, months[tm.tm_mon]);
        else if (c1 == 'M' && c2 == '3') snprintf(v, 4, "%s", months[tm.tm_mon]);
        else if (c1 == 'M' && c2 == 'N') num = tm.tm_mon + 1;
        else if (c1 == 'C' && c2 == 'E') num = (tm.tm_year + 1900) / 100;
        else if (c1 == 'Y' && c2 == 'R') num = tm.tm_year % 100;
        else if (c1 == 'W' && c2 == 'K') num = tm.tm_yday / 7 + 1;
        else if (c1 == 'D' && c2 == 'N') { num = tm.tm_yday + 1; two = 3; }
        else { out[o++] = '%'; continue; }
        if (num >= 0) snprintf(v, sizeof v, z || !two ? "%d" : two == 3 ? "%03d" : "%02d", num);
        for (char *q = v; *q && o < 200; q++) out[o++] = *q;
        p += 2;
    }
    out[o] = 0;
    if ((uint32_t)o + 1 > R[2]) { VF = 1; return; }
    memcpy(MEM + R[1], out, o + 1);
    R[0] = R[1]; R[1] += o; R[2] -= o + 1;
}

/* OS_SWINumberFromString: R1 the name (control-terminated) -> R0 its
 * number. Only the names programs look up to find out what the OS can do:
 * the RISC OS 3.5 ones exist only on a Risc PC (vidc20). */
static void swi_number_from_string(void)
{
    static const struct { const char *name; uint32_t n; int ro35; } names[] = {
        {"OS_ScreenMode", 0x65, 1}, {"OS_Memory", 0x68, 1}, {"OS_DynamicArea", 0x66, 1},
        {"OS_ReadDynamicArea", 0x5C, 0}, {"OS_ChangeDynamicArea", 0x2A, 0},
        {"OS_CheckModeValid", 0x3F, 0}, {"OS_ReadModeVariable", 0x35, 0},
        {"OS_ReadVduVariables", 0x31, 0}, {"OS_SpriteOp", 0x2E, 0},
        {"OS_WriteC", 0x00, 0}, {"OS_WriteS", 0x01, 0}, {"OS_Write0", 0x02, 0}, {"OS_NewLine", 0x03, 0},
        {"OS_ReadC", 0x04, 0}, {"OS_CLI", 0x05, 0}, {"OS_Byte", 0x06, 0}, {"OS_Word", 0x07, 0},
        {"OS_File", 0x08, 0}, {"OS_Args", 0x09, 0}, {"OS_BGet", 0x0A, 0}, {"OS_BPut", 0x0B, 0},
        {"OS_GBPB", 0x0C, 0}, {"OS_Find", 0x0D, 0}, {"OS_ReadLine", 0x0E, 0}, {"OS_GetEnv", 0x10, 0},
        {"OS_Exit", 0x11, 0}, {"OS_Module", 0x1E, 0}, {"OS_Mouse", 0x1C, 0}, {"OS_ReadUnsigned", 0x21, 0},
        {"OS_ReadVarVal", 0x23, 0}, {"OS_SetVarVal", 0x24, 0}, {"OS_GSTrans", 0x27, 0},
        {"OS_ReadPalette", 0x2F, 0}, {"OS_ValidateAddress", 0x3A, 0}, {"OS_ReadMonotonicTime", 0x42, 0},
        {"OS_RemoveCursors", 0x36, 0}, {"OS_RestoreCursors", 0x37, 0}, {"OS_EnterOS", 0x16, 0},
        {"OS_ReadMemMapInfo", 0x51, 0}, {"OS_ReadRAMFsLimits", 0x55, 0}, {"RamFS_FreeSpace", 0x40783, 0}, {"OS_SerialOp", 0x57, 0},
        {"OS_ConvertStandardDateAndTime", 0xC0, 0}, {"OS_ConvertDateAndTime", 0xC1, 0},
        {"Sound_Configure", 0x40140, 0}, {"Sound_Enable", 0x40141, 0}, {"Sound_Volume", 0x40180, 0},
        {"Sound_SoundLog", 0x40181, 0}, {"Sound_AttachVoice", 0x40185, 0}, {"Sound_Control", 0x40189, 0},
        {"Sound_AttachNamedVoice", 0x4018A, 0}, {"Sound_ReadControlBlock", 0x4018B, 0},
        {"Sound_Tuning", 0x40182, 0}, {"Sound_Speaker", 0x40143, 0}, {"Sound_InstallVoice", 0x40183, 0},
        {"Hourglass_On", 0x406C0, 0}, {"Hourglass_Off", 0x406C1, 0}, {"Hourglass_Smash", 0x406C2, 0},
        {"Wimp_Initialise", 0x400C0, 0}, {"Wimp_Poll", 0x400C7, 0}, {"Wimp_CloseDown", 0x400DD, 0},
    };
    char s[64]; int l = 0;
    while (l < 63 && MEM[R[1] + l] >= ' ') { s[l] = MEM[R[1] + l]; l++; }
    s[l] = 0;
    for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++)
        if (!strcmp(s, names[k].name) && (!names[k].ro35 || rg->vidc20)) { R[0] = names[k].n; return; }
    if (modules_swi_number(s, &R[0]) || fonts_swi_number(s, &R[0])) return;
    VF = 1;                                      /* "SWI name not known" */
}

/* The modes OS_ScreenMode 2 lists: what the monitor definition file of a
 * multisync monitor offers, at the depths VIDC20 shows - the Archimedes'
 * 50Hz TV-rate resolutions and the multisync ones (all of which libroger
 * shows), 1 to 32 bits per pixel. */
static const struct { uint16_t w, h, hz; } enum_modes[] = {
    {320, 256, 50}, {640, 256, 50}, {640, 512, 60}, {640, 480, 60}, {800, 600, 60},
};

/* OS_ScreenMode 2: enumerate the available modes. R2 entries are skipped,
 * then each is written at R6 (format 1: +0 size, +4 flags 1, +8 x pixels,
 * +12 y pixels, +16 log2 bpp, +20 frame rate, +24 name; no names here) while
 * R7 has room; R2 counts down through both, R6/R7 move past what's written.
 * R6 = 0 only counts: R7 goes down by the space needed. R1 = 0 when the list
 * ran out (else preserved: call again from the new R2). */
static void enumerate_modes(void)
{
    for (unsigned k = 0; k < sizeof enum_modes / sizeof enum_modes[0]; k++)
        for (int l2 = 0; l2 <= 5; l2++) {
            struct rg_mode m = {enum_modes[k].w, enum_modes[k].h, 1 << l2};
            if (mode_bytes(&m) > 0x200000) continue;
            if ((int32_t)R[2] > 0) { R[2]--; continue; }
            if (R[6] && R[7] < 28) return;
            if (R[6]) {
                uint32_t e[7] = {28, 1, m.width, m.height, (uint32_t)l2, enum_modes[k].hz, 0};
                for (int i = 0; i < 7; i++) WR32(R[6] + 4 * i, e[i]);
                R[6] += 28;
            }
            R[7] -= 28;
            R[2]--;
        }
    R[1] = 0;
}

/* OS_ScreenMode (Risc PC): 0 select mode R1 (a number or a selector), 1
 * read the current one into R1, 2 enumerate the modes */
static int screen_mode_swi(void)
{
    if (!rg->vidc20) return 0;
    if (R[0] == 2) ev("OS_ScreenMode", "iii", R[0], R[1], R[2]);
    else ev("OS_ScreenMode", "ii", R[0], R[1]);
    if (R[0] == 0) { if (R[1] < 0x100) { vdu_put(22); vdu_put(R[1]); } else if (!video_set_mode(R[1])) VF = 1; }
    else if (R[0] == 1) R[1] = screen_mode;
    else if (R[0] == 2) enumerate_modes();
    return 1;
}

/* OS_Memory (Risc PC): reason 8, the amount of memory of a type (bits 8-11:
 * 1 DRAM, 2 VRAM, 3 ROM) as R1 pages of R2 bytes: 2MB of VRAM */
static int os_memory(void)
{
    if (!rg->vidc20 || (R[0] & 0xFF) != 8) return 0;
    uint32_t type = R[0] >> 8 & 0xF;
    R[1] = type == 2 ? 512 : type == 1 ? 4096 : type == 3 ? 1024 : 0;
    R[2] = 4096;
    return 1;
}

/* A SWI's error: an error block (number, message) for R0, V set. (The non-X
 * form's behaviour - the error handler - is the caller's to decide.) */
void os_error(uint32_t num, const char *msg)
{
    WR32(ERROR_BUF, num);
    size_t l = strlen(msg);
    if (l > 250) l = 250;
    memcpy(MEM + ERROR_BUF + 4, msg, l);
    MEM[ERROR_BUF + 4 + l] = 0;
    R[0] = ERROR_BUF;
    VF = 1;
}

/* Territory_ReadSymbols: R0 the territory (-1 the current: the UK's), R1
 * what: 0 decimal point, 1 thousands separator, 2 their grouping, 3
 * international currency symbol, 4 currency symbol, 5 its decimal point, 6
 * its thousands separator, 7 its grouping, 8 positive sign, 9 negative
 * sign, 10 international fractional digits, 11 fractional digits, 12-15
 * currency before positive/negative, separated by a space: 1, 0, 1, 0;
 * 16/17 sign position: 1 -> R0 (a string, or a value) */
static void territory_symbols(void)
{
    static const char *const str[] = {".", ",", "\3", "GBP ", "\xA3", ".", ",", "\3", "", "-"};
    static const uint32_t val[] = {2, 2, 1, 0, 1, 0, 1, 1};
    if (R[1] < 10) {
        uint32_t a = MOD_POSTFIX + 8 + 8 * R[1];
        strcpy((char *)MEM + a, str[R[1]]);
        R[0] = a;
    } else if (R[1] < 18) R[0] = val[R[1] - 10];
    else os_error(0x43062, "Unknown symbol");
}

/* OS_CRC: R0 the CRC so far, R1/R2 the block [start, end), R3 the step
 * between the bytes taken -> R0. The CRC-16 of the BBC's cassette and ROM
 * filing systems, which RISC OS keeps: polynomial &1021, top bit first. */
static void os_crc(void)
{
    uint32_t crc = R[0] & 0xFFFF;
    int32_t step = (int32_t)R[3] ? (int32_t)R[3] : 1;
    for (uint32_t a = R[1]; step > 0 ? a < R[2] : a > R[2]; a += step) {
        crc ^= (uint32_t)MEM[a & 0x3FFFFFF] << 8;
        for (int k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
    R[0] = crc;
}

/* Transient callbacks (OS_AddCallBack: R0 the routine, R1 its R12;
 * OS_RemoveCallBack: the same), run when a SWI or an interrupt returns to
 * USR mode with interrupts enabled - in SVC mode, interrupts disabled,
 * returning with MOV PC,R14 - the caller's registers kept (a ticker event
 * that adds one, to do its work outside the interrupt, has it run as soon
 * as the interrupt is over, whatever the program is doing) */
#define MAXCALLBACK 16
static uint32_t callback[MAXCALLBACK][2];
static int ncallback;

void run_callbacks(void)
{
    while (ncallback && (MODEBITS & 3) == 0 && !(MODEBITS & 0x08000000u)) {
        uint32_t a = callback[0][0], r12 = callback[0][1];
        memmove(callback, callback + 1, --ncallback * sizeof *callback);
        uint32_t saved[16], f[4] = {NF, ZF, CF, VF}, snpc = npc;
        memcpy(saved, R, sizeof saved);
        ev("callback", "ii", a, r12);
        R[12] = r12;
        call_code(a, MODE_SVC | 0x08000000u, HANDLER_RET | MODE_SVC | 0x08000000u);
        memcpy(R, saved, sizeof saved);
        NF = f[0]; ZF = f[1]; CF = f[2]; VF = f[3];
        npc = snpc;
    }
}

static void swi_body(uint32_t n, uint32_t nextpc);

/* The physical memory, as MEMC sees it (OS_ReadMemMapEntries): 128 pages
 * of 32K. Screen memory is physical memory from 0 (video DMA starts
 * there), the page at &78000 holds the OS's sound buffers, logical
 * &1F00000 (sound.c's DMA), and the application slot takes the pages
 * after those; the rest are unused, mapped past the sound page */
static uint32_t page_logical(uint32_t page, uint32_t *ppl)
{
    uint32_t phys = page * 0x8000, first_app = (screen_size() + 0x7FFF) / 0x8000;
    *ppl = 0;
    if (phys == 0x78000) { *ppl = 1; return 0x1F00000; }
    if (phys < screen_size()) return screen_base() + phys;
    if (first_app <= 15) first_app = 16;
    if (page >= first_app && LOAD + (page - first_app) * 0x8000 < ram_limit()) return LOAD + (page - first_app) * 0x8000;
    *ppl = 3;
    return 0x1F08000;
}

/* A SWI instruction. The ARM's SWI vector, &8: RISC OS's leads to its SWI
 * dispatch (here, swi()); a program may put its own branch there, to see
 * every SWI first (Phaethon's loader does, as copy protection, putting the
 * OS's word back around the SWIs it then makes itself). The SWI enters it
 * as the ARM does: SVC mode, IRQs off, R14_svc the return address with the
 * caller's PSR. 1 if so: the caller leaves for npc. */
int swi_insn(uint32_t n, uint32_t nextpc)
{
    uint32_t to = hw_vector(8);
    if (!to || to == OS_SWI_ENTRY) { swi(n, nextpc); return 0; }
    uint32_t ret = nextpc | PSR();
    ev("swi_vector", "ii", n, to);
    set_psr_mode(0x08000000u | 3);
    R[14] = ret;
    npc = to;
    return 1;
}

/* The kernel's SWI handler, where the OS's branch at &8 leads: entered as
 * the SWI vector enters it (SVC mode, R14 the caller's return address and
 * PSR), it makes the SWI in the instruction before R14 and returns to the
 * caller, in its mode, with V saying whether there was an error. A program's
 * own handler at &8 passes SWIs on to it so (Phaethon's jumps to where the
 * word it replaced led). */
void os_swi_entry(void)
{
    uint32_t ret = R[14];
    npc = ret & 0x3FFFFFC;
    swi(RD32(npc - 4) & 0xFFFFFF, npc);
    uint32_t v = VF;
    SETPSR(ret);
    VF = v;
}

void swi(uint32_t n, uint32_t nextpc)
{
    int user = (MODEBITS & 3) == 0;
    static int trace = -1;                       /* ROGER_SWITRACE=1: each SWI to stderr, and its error */
    if (trace < 0) trace = getenv("ROGER_SWITRACE") != NULL;
    uint32_t in[4] = {R[0], R[1], R[2], R[3]};
    swi_body(n, nextpc);
    if (trace) {
        fprintf(stderr, "%llu swi %06x from %08x (%08x %08x %08x %08x)", (unsigned long long)icount, n, nextpc - 4, in[0], in[1], in[2], in[3]);
        if (VF && R[0] < MEMSIZE - 256) fprintf(stderr, " -> error %x %.80s", RD32(R[0]), (char *)MEM + R[0] + 4);
        else fprintf(stderr, " -> %08x %08x %08x %08x", R[0], R[1], R[2], R[3]);
        if ((n & ~0x20000u) == 0x400C7 || (n & ~0x20000u) == 0x400E1 || (n & ~0x20000u) == 0x400E7 || (n & ~0x20000u) == 0x400CB)
            for (int k = 0; k < 8; k++) fprintf(stderr, " %x", RD32(R[1] + 4 * k));
        if ((n & ~0x20000u) == 7)                                       /* OS_Word: the block */
            for (int k = 0; k < 9; k++) fprintf(stderr, " %02x", MEM[R[1] + k]);
        fputc('\n', stderr);
    }
    if (user) run_callbacks();
}

static void swi_body(uint32_t n, uint32_t nextpc)
{
    uint32_t x = n & 0x20000, b = n & ~0x20000u;
    VF = 0;
    swi_pc = nextpc;
    /* A SWI loads R14_svc with the return address - visible to the caller
     * only if it's itself in SVC mode (why module code stacks R14 first). */
    if ((MODEBITS & 3) == 3) R[14] = nextpc | PSR(); else bank_r14[3] = nextpc | PSR();
    swi_x = x != 0;
    if (rg->swi && rg->swi(b, x != 0)) return;
    if (b >= 0x100 && b <= 0x1FF) { vdu_put(b - 0x100); return; }   /* OS_WriteI */
    switch (b) {
    case 0: vdu_put(R[0]); return;                                  /* OS_WriteC */
    case 1: {                                                       /* OS_WriteS */
        char s[1100]; int l = cstr(nextpc, s, 0);
        for (int k = 0; k < l; k++) vdu_put(s[k]);
        npc = (nextpc + l + 4) & ~3u;
        return;
    }
    case 2: {                                                       /* OS_Write0 */
        char s[1100]; int l = cstr(R[0], s, 0);
        for (int k = 0; k < l; k++) vdu_put(s[k]);
        R[0] += l + 1;
        return;
    }
    case 3: vdu_put('\n'); vdu_put('\r'); return;                   /* OS_NewLine */
    case 4:                                      /* OS_ReadC: wait for a key; C set on Escape */
        CF = 0;
        if (!rg_interactive) {                   /* headless: a key typed (type:), waited for in */
            uint32_t m = MODEBITS;               /* emulated time while the script has some to */
            MODEBITS &= ~0x08000000u;            /* come (interrupts on, as the kernel waits); */
            for (;;) {                           /* or Return */
                int c = key_buffer_get();
                if (c >= 0) { R[0] = (uint32_t)c; break; }
                if (!script_typing()) { R[0] = 13; break; }
                roger_idle(TICK_STEPS);
            }
            MODEBITS = m;
            return;
        }
        for (;;) {
            if (escape_pressed) { R[0] = 27; CF = 1; return; }
            int c = key_buffer_get();
            if (c >= 0) { R[0] = (uint32_t)c; return; }
            display_sleep_ns(CS_NS);
            rt_service(0);
        }
    case 0x46: for (uint32_t k = 0; k < R[1]; k++) vdu_put(MEM[R[0] + k]); return;   /* OS_WriteN */
    case 0x45: vdu_put(25); vdu_put(R[0]); vdu_put(R[1]); vdu_put(R[1] >> 8);       /* OS_Plot */
               vdu_put(R[2]); vdu_put(R[2] >> 8); return;
    case 0x36: case 0x37: return;                                   /* OS_RemoveCursors, OS_RestoreCursors */
    case 6: os_byte(); return;
    case 7: os_word(); return;
    case 0x10: {                                 /* OS_GetEnv: command line, RAM limit, start time */
        const char *cmd = rg->command_line ? rg->command_line : "!RunImage";
        size_t l = strlen(cmd);
        if (l > 0xFF) l = 0xFF;
        memcpy(MEM + ENV_AREA, cmd, l);
        MEM[ENV_AREA + l] = 0;
        memset(MEM + ENV_AREA + 0x100, 0, 5);
        R[0] = ENV_AREA; R[1] = ram_limit(); R[2] = ENV_AREA + 0x100;
        return;
    }
    case 0x1C:                                   /* OS_Mouse: pointer confined to the bounding box */
        if (rg_interactive || script_active()) {      /* (headless: --script's mouse) */
                              R[0] = clamp_i(mouse_x, mbox[0], mbox[2]); R[1] = clamp_i(mouse_y, mbox[1], mbox[3]);
                              R[2] = mouse_buttons; R[3] = monotonic; }
        else { R[0] = 640; R[1] = 512; R[2] = 0; R[3] = monotonic; }
        return;
    case 0x40: {                                 /* OS_ChangeEnvironment: R0 handler, R1 code, R2 R12, R3 buffer */
                                                 /* returns the previous values; 0 = read only */
        ev("OS_ChangeEnvironment", "iiii", R[0], R[1], R[2], R[3]);
        if (R[0] <= 16) {
            uint32_t *e = env[R[0]], old[3] = {e[0], e[1], e[2]};
            /* MemoryLimit (0) and ApplicationSpace (14) hold addresses: the
               end of the application slot (the RAM limit), unless set */
            if ((R[0] == 0 || R[0] == 14) && !old[0]) old[0] = ram_limit();
            for (int k = 0; k < 3; k++) if (R[1 + k]) e[k] = R[1 + k];
            R[1] = old[0]; R[2] = old[1]; R[3] = old[2];
        }
        return;
    }
    case 0x58:                                   /* OS_ReadSysInfo */
        if (R[0] == 2) {                         /* 2: the hardware - R0 its chips (bits 8-15 the I/O */
            R[0] = rg->vidc20 ? 0x01010100u : 0; /* controller, 16-23 the memory controller, 24-31 the */
            R[1] = R[2] = R[3] = R[4] = 0;       /* video controller: IOC, MEMC1, VIDC1 0, IOMD, */
            return;                              /* VIDC20 1), no 82C710, no LCD, no machine ID */
        }
        if (R[0] == 0) { R[0] = rg->screen_size ? rg->screen_size : 0x14000; return; }   /* 0: configured screen size */
        if (R[0] == 1) {                         /* 1: configured mode, monitor type, sync (0 separate) */
            R[0] = rg->initial_mode; R[1] = rg->monitor_type; R[2] = 0;
            return;
        }
        break;
    case 0x1A: {                                 /* OS_UpdateMEMC: R0 bits, R1 mask; R0 the old value */
        uint32_t old = memc_control;
        memc_control = (old & ~R[1]) | (R[0] & R[1] & 0x1FFF);
        R[0] = old;
        return;
    }
    case 0x13: MODEBITS &= ~0x08000000u; return; /* OS_IntOn / OS_IntOff: the caller's I bit */
    case 0x14: MODEBITS |= 0x08000000u; return;
    case 0x51: R[0] = 0x8000; R[1] = 128; return;   /* OS_ReadMemMapInfo: 128 pages of 32K (4MB) */
    case 0x52:                                   /* OS_ReadMemMapEntries: R0 entries {physical page, */
        /* logical address, protection}, ending with -1 - or with any number
           that isn't one of the 128 pages (Desktop Tracker passes a single
           entry, on its stack, unterminated) */
        for (uint32_t b = R[0]; RD32(b) < 128; b += 12) {
            uint32_t ppl;
            WR32(b + 4, page_logical(RD32(b), &ppl));
            WR32(b + 8, ppl);
        }
        return;
    case 0x56: ev("OS_SetECFOrigin", "ii", R[0], R[1]); return;
    case 0x16: set_psr_mode((MODEBITS & ~3u) | 3); return;   /* OS_EnterOS: caller continues in SVC mode */
    case 0x6D:                                   /* OS_PlatformFeatures (RISC OS 3.7) */
        if (!rg->strongarm) break;
        if (R[0] == 0) { R[0] = 1; return; }     /* 0: code areas must be synchronised (StrongARM) */
        break;
    case 0x2C: CF = escape_pressed != 0; return;   /* OS_ReadEscapeState (headless: a script's esc) */
    case 0x31: {                                 /* OS_ReadVduVariables */
        uint32_t i = R[0], o = R[1];
        for (;;) { uint32_t v = RD32(i); if (v == 0xFFFFFFFF) break;
                   WR32(o, vduvar(v)); i += 4; o += 4; }
        return;
    }
    case 0x3F:                                   /* OS_CheckModeValid: R0 mode; -1 and C set if not */
        if (!mode_info(R[0]) || (R[0] > 127 && R[0] < 0x100)) { R[0] = 0xFFFFFFFF; CF = 1; } else CF = 0;
        return;
    case 0x39: swi_number_from_string(); return;
    case 0x55:                                   /* OS_ReadRAMFsLimits: start, end (equal: none) */
        R[0] = 0x01000000; R[1] = R[0] + rg->ramfs_size; return;
    case 0x57:                                   /* OS_SerialOp: no serial port - nothing arrives, */
        if (R[0] == 4) CF = 1;                   /* nothing is sent, settings read back as 0 */
        else if (R[0] != 3) R[1] = 0;
        return;
    case 0x3A:                                   /* OS_ValidateAddress: [R0, R1) mapped -> C clear. Everything
                                                    below 64M is (application space, RMA, screen,
                                                    dynamic areas) except between the RAM limit and RMA */
        CF = !(R[0] <= R[1] && R[1] <= MEMSIZE && (R[1] <= ram_limit() || R[0] >= 0x1800000));
        return;
    case 0x21: {                                 /* OS_ReadUnsigned: R0 flags + base, R1 string -> R1 past it, R2 value */
        uint32_t base = R[0] & 0xFF ? R[0] & 0xFF : 10, p = R[1], v = 0, lim = 0xFFFFFFFF, b = 0, q;
        while (MEM[p] == ' ') p++;               /* leading spaces are skipped */
        q = p;
        if (MEM[p] == '&') { base = 16; p++; }
        else {                                   /* "base_digits" */
            while (MEM[q] >= '0' && MEM[q] <= '9') b = b * 10 + MEM[q++] - '0';
            if (q > p && MEM[q] == '_' && b >= 2 && b <= 36) { base = b; p = q + 1; }
        }
        int n = 0;
        for (;; p++, n++) {
            int c = MEM[p] | 0x20, d = MEM[p] >= '0' && MEM[p] <= '9' ? MEM[p] - '0'
                                     : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
            if (d >= (int)base) break;
            v = v * base + d;
        }
        if (R[0] & 0x40000000u) lim = 255;       /* bit 30: 0-255; bit 29: 0-R2 */
        if (R[0] & 0x20000000u) lim = R[2];
        if (!n || v > lim || ((R[0] & 0x80000000u) && MEM[p] >= ' ')) { if (swi_x) VF = 1; return; }
        R[1] = p; R[2] = v;
        return;
    }
    case 0x34:                                   /* OS_CallAVector: R9 the vector */
        if (R[9] == 0x23 && video_palettev()) return;   /* PaletteV, nobody claiming it */
        ev("OS_CallAVector", "ii", R[9], R[4]);
        return;
    case 0x35:                                   /* OS_ReadModeVariable: R0 mode (-1 current), R1 variable */
        R[2] = modevar((int32_t)R[0] < 0 ? -1 : (int64_t)R[0], R[1]);
        return;
    case 0x11:                                   /* OS_Exit: a loader program (a utility *Run */
        if (in_loader) { npc = RETURN_TO_OS; return; }   /* from !Run) ends, and !Run goes on */
        finish("OS_Exit");
    case 0x27: {                                 /* OS_GSTrans: R0 string, R1 buffer, R2 size (bit 29: */
        char in[1024], out[1024];                /* a space ends the string too) -> R0 past it, R2 length */
        uint32_t p = R[0];
        int n = 0, sp = (R[2] & 0x20000000u) != 0, size = (int)(R[2] & 0x1FFFFFFF);
        while (n < 1023 && MEM[p] >= ' ' && !(sp && MEM[p] == ' ')) in[n++] = MEM[p++];
        in[n] = 0;
        gstrans(in, out, sizeof out);
        int l = (int)strlen(out);
        CF = l >= size;                          /* the buffer overflowed */
        if (CF) l = size > 0 ? size - 1 : 0;
        memcpy(MEM + R[1], out, l);
        if (size > l) MEM[R[1] + l] = 0;
        R[0] = p; R[2] = (uint32_t)l;
        return;
    }
    case 0x2B: finish("OS_GenerateError");
    case 0x42: R[0] = rg_interactive ? (uint32_t)((display_ticks_ns() - rg_t0_ns) / CS_NS)   /* OS_ReadMonotonicTime */
                                     : (uint32_t)(icount / TICK_STEPS); return;
    }
    if (b >= 0xD0 && b <= 0xE8) { convert(b); return; }
    if (b == 0x28) { binary_to_decimal(); return; }                 /* OS_BinaryToDecimal */
    if (b == 0xC0 || b == 0xC1) { convert_time(b); return; }
    if (b == 0x40783 && rg->ramfs_size) {        /* RamFS_FreeSpace: R0 free, R1 the largest object */
        uint32_t used = ramfs_used();
        R[0] = R[1] = used < rg->ramfs_size ? rg->ramfs_size - used : 0;
        return;
    }
    if (b >= 0x406C0 && b <= 0x406C7) return;   /* Hourglass: nothing to show */
    if ((b == 0x65 && screen_mode_swi()) || (b == 0x68 && os_memory())) return;
    if (joystick_swi(b) || cli_swi(b, x != 0) || clib_swi(b) || files_swi(b) || disc_swi(b) || irq_swi(b) || sound_swi(b) || modules_swi(b, x != 0) || wimp_swi(b) || fonts_swi(b)
        || resourcefs_swi(b) || messagetrans_swi(b, x != 0) || colourtrans_swi(b) || jpeg_swi(b) || buffers_swi(b) || heap_swi(b)
        || (rg->vidc20 && dynarea_swi(b))) return;
    if (b == 0x43040) { R[0] = 1; return; }     /* Territory_Number: the UK */
    if (b == 0x4305E) { territory_symbols(); return; }
    if (b == 0x5B) { os_crc(); return; }
    if (b == 0x54) {                             /* OS_AddCallBack */
        if (ncallback < MAXCALLBACK) { callback[ncallback][0] = R[0] & 0x3FFFFFC; callback[ncallback++][1] = R[1]; }
        return;
    }
    if (b == 0x5F) {                             /* OS_RemoveCallBack */
        for (int k = 0; k < ncallback; k++)
            if (callback[k][0] == (R[0] & 0x3FFFFFC) && callback[k][1] == R[1]) {
                memmove(callback + k, callback + k + 1, (--ncallback - k) * sizeof *callback);
                break;
            }
        return;
    }
    if (b == 0x6E) return;                       /* OS_SynchroniseCodeAreas: stores to code are seen anyway */
    char name[48]; sprintf(name, "unhandled_SWI_%x", n);
    ev(name, "iii", R[0], R[1], R[2]);
    if (!x) finish("unhandled SWI");
    os_error(0x1E6, "SWI not known");
}
