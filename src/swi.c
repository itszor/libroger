/* SWI dispatch and the kernel's own SWIs: VDU output, OS_Byte, OS_Word,
 * the environment, conversions. Files, sound, vectors and modules have
 * their own files. Every call worth knowing about is written to the event
 * log (roger_log()). */
#include <string.h>
#include <strings.h>
#include <time.h>
#include "internal.h"

uint32_t swi_pc;

/* INKEY -(inkey): host keyboard (by internal key number + 1), or in
 * headless runs the scripted keys */
int pressed(int inkey)
{
    if (inkey >= 1 && inkey <= 128 && key_down[inkey - 1]) return 1;   /* (headless: --script keys) */
    if (rg_interactive) return 0;
    for (unsigned k = 0; k < rg->nkeys; k++)
        if (rg->keys[k].inkey == inkey && rg->keys[k].from <= icount && icount < rg->keys[k].to) return 1;
    return 0;
}

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void os_byte(void)
{
    uint32_t op = R[0] & 0xFF;
    if (op == 129) {                                   /* INKEY */
        if (R[2] == 0xFF && (R[1] & 0xFF) == 0) {      /* the OS version: 3.1, or 3.5 on a Risc PC */
            R[1] = rg->vidc20 ? 0xA5 : 0xA4; R[2] = 0;
        } else if (R[2] == 0xFF && (R[1] & 0xFF) >= 0x80) {   /* scan one key (R1's low byte) */
            int k = 256 - (int)(R[1] & 0xFF);
            R[1] = R[2] = pressed(k) ? 0xFF : 0;
        } else if (rg_interactive) {                    /* read a key, with time limit */
            uint64_t until = display_ticks_ns() + (uint64_t)((R[2] & 0x7F) << 8 | R[1]) * CS_NS;
            for (;;) {
                if (escape_pressed) { R[1] = 0; R[2] = 27; return; }
                int c = key_buffer_get();
                if (c >= 0) { R[1] = c; R[2] = 0; return; }
                if (display_ticks_ns() >= until) { R[1] = 0; R[2] = 0xFF; return; }
                display_sleep_ns(CS_NS);
                rt_service(0);
            }
        } else {
            R[1] = 0; R[2] = 0xFF;
            for (unsigned k = 0; k < rg->nkeys; k++)   /* a scripted Space reads as ' ' */
                if (rg->keys[k].from <= icount && icount < rg->keys[k].to && rg->keys[k].inkey == 99) {
                    R[1] = 32; R[2] = 0;
                }
        }
        return;
    }
    if (op == 121) {                                   /* keyboard scan: R1 key ^ &80 (that key), */
        if (R[1] & 0x80) { R[1] = pressed((R[1] ^ 0x80) + 1) ? 0xFF : 0; return; }
        uint32_t k = R[1];                             /* or the first key from R1 up pressed */
        while (k < 128 && !pressed(k + 1)) k++;
        R[1] = k < 128 ? k : 0xFF;
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

static void os_word(void)
{
    uint8_t *blk = MEM + R[1];
    if (R[0] == 14 && blk[0] == 3) {                 /* read the RTC: 5 bytes, centiseconds */
        uint64_t t = rtc_cs();
        for (int k = 0; k < 5; k++) blk[k] = (uint8_t)(t >> 8 * k);
        return;
    }
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
        {"OS_ReadMemMapInfo", 0x51, 0}, {"OS_ReadRAMFsLimits", 0x55, 0}, {"OS_SerialOp", 0x57, 0},
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
    if (modules_swi_number(s, &R[0])) return;
    VF = 1;                                      /* "SWI name not known" */
}

/* OS_ScreenMode (Risc PC): 0 select mode R1 (a number or a selector), 1
 * read the current one into R1 */
static int screen_mode_swi(void)
{
    if (!rg->vidc20) return 0;
    ev("OS_ScreenMode", "ii", R[0], R[1]);
    if (R[0] == 0) { if (R[1] < 0x100) { vdu_put(22); vdu_put(R[1]); } else if (!video_set_mode(R[1])) VF = 1; }
    else if (R[0] == 1) R[1] = screen_mode;
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

void swi(uint32_t n, uint32_t nextpc)
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
        if (!rg_interactive) { R[0] = 13; return; }   /* headless: Return */
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
        static uint32_t env[17][3];              /* returns the previous values; 0 = read only */
        ev("OS_ChangeEnvironment", "iiii", R[0], R[1], R[2], R[3]);
        if (R[0] <= 16) {
            uint32_t *e = env[R[0]], old[3] = {e[0], e[1], e[2]};
            for (int k = 0; k < 3; k++) if (R[1 + k]) e[k] = R[1 + k];
            R[1] = old[0]; R[2] = old[1]; R[3] = old[2];
        }
        return;
    }
    case 0x1D: return;                           /* OS_RemoveCursors (old number): no cursor to hide */
    case 0x13: MODEBITS &= ~0x08000000u; return; /* OS_IntOn / OS_IntOff: the caller's I bit */
    case 0x14: MODEBITS |= 0x08000000u; return;
    case 0x51: R[0] = 0x8000; R[1] = 128; return;   /* OS_ReadMemMapInfo: 128 pages of 32K (4MB) */
    case 0x56: ev("OS_SetECFOrigin", "ii", R[0], R[1]); return;
    case 0x16: set_psr_mode((MODEBITS & ~3u) | 3); return;   /* OS_EnterOS: caller continues in SVC mode */
    case 0x2C: CF = rg_interactive ? escape_pressed : 0; return;   /* OS_ReadEscapeState */
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
    case 0x55: R[0] = R[1] = 0x01000000; return;   /* OS_ReadRAMFsLimits: no RAM disc (start = end) */
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
    case 0x11: finish("OS_Exit");
    case 0x2B: finish("OS_GenerateError");
    case 0x42: R[0] = rg_interactive ? (uint32_t)((display_ticks_ns() - rg_t0_ns) / CS_NS)   /* OS_ReadMonotonicTime */
                                     : (uint32_t)(icount / TICK_STEPS); return;
    }
    if (b >= 0xD0 && b <= 0xE8) { convert(b); return; }
    if (b == 0xC0 || b == 0xC1) { convert_time(b); return; }
    if (b >= 0x406C0 && b <= 0x406C7) return;   /* Hourglass: nothing to show */
    if ((b == 0x65 && screen_mode_swi()) || (b == 0x68 && os_memory())) return;
    if (cli_swi(b, x != 0) || files_swi(b) || irq_swi(b) || sound_swi(b) || modules_swi(b, x != 0) || wimp_swi(b)) return;
    char name[48]; sprintf(name, "unhandled_SWI_%x", n);
    ev(name, "iii", R[0], R[1], R[2]);
    if (!x) finish("unhandled SWI");
    VF = 1;
}
