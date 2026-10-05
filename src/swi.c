/* SWI dispatch and the kernel's own SWIs: VDU output, OS_Byte, OS_Word,
 * the environment, conversions. Files, sound, vectors and modules have
 * their own files. Every call worth knowing about is written to the event
 * log (roger_log()). */
#include <string.h>
#include <strings.h>
#include "internal.h"

uint32_t swi_pc;

/* INKEY -(inkey): host keyboard (by internal key number + 1), or in
 * headless runs the scripted keys */
int pressed(int inkey)
{
    if (rg_interactive) return inkey >= 1 && inkey <= 128 && key_down[inkey - 1];
    for (unsigned k = 0; k < rg->nkeys; k++)
        if (rg->keys[k].inkey == inkey && rg->keys[k].from <= icount && icount < rg->keys[k].to) return 1;
    return 0;
}

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void os_byte(void)
{
    uint32_t op = R[0] & 0xFF;
    if (op == 129) {                                   /* INKEY */
        if (R[2] == 0xFF && R[1] >= 0x80) {            /* scan one key */
            int k = 256 - R[1];
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
    if (op == 121) {                                   /* keyboard scan */
        if (R[1] & 0x80) R[1] = pressed((R[1] ^ 0x80) + 1) ? 0xFF : 0;
        else R[1] = 0xFF;
        return;
    }
    if (op == 112 || op == 113) {                      /* screen bank for VDU drivers / display */
        video_set_bank(op == 113, (R[1] >= 1 ? R[1] - 1 : 0) & 1);
        ev("OS_Byte", "ii", op, R[1]);
        return;
    }
    if (op == 13 || op == 14) {                        /* disable / enable event */
        uint32_t m = 1u << (R[1] & 31);
        ev("OS_Byte", "iii", op, R[1], R[2]);
        R[1] = (events_enabled & m) ? 1 : 0;              /* old state */
        if (op == 14) events_enabled |= m; else events_enabled &= ~m;
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
    if (op == 135) { R[1] = 0; R[2] = screen_mode; return; }   /* character at cursor, screen mode */
    /* OS_Byte 15 (flush all / current input buffer), 21 (flush buffer r1; 0 = keyboard) */
    if (rg_interactive && (op == 15 || (op == 21 && R[1] == 0))) key_buffer_flush();
    ev("OS_Byte", "iii", op, R[1], R[2]);
}

static void os_word(void)
{
    uint8_t *blk = MEM + R[1];
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
        R[0] = ENV_AREA; R[1] = LOAD + rg->wimpslot; R[2] = ENV_AREA + 0x100;
        return;
    }
    case 0x1C:                                   /* OS_Mouse: pointer confined to the bounding box */
        if (rg_interactive) { R[0] = clamp_i(mouse_x, mbox[0], mbox[2]); R[1] = clamp_i(mouse_y, mbox[1], mbox[3]);
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
    case 0x16: set_psr_mode((MODEBITS & ~3u) | 3); return;   /* OS_EnterOS: caller continues in SVC mode */
    case 0x2C: CF = rg_interactive ? escape_pressed : 0; return;   /* OS_ReadEscapeState */
    case 0x31: {                                 /* OS_ReadVduVariables */
        uint32_t i = R[0], o = R[1];
        for (;;) { uint32_t v = RD32(i); if (v == 0xFFFFFFFF) break;
                   WR32(o, vduvar(v)); i += 4; o += 4; }
        return;
    }
    case 0x35:                                   /* OS_ReadModeVariable: R0 mode (-1 current), R1 variable */
        R[2] = modevar((int32_t)R[0] < 0 ? -1 : (int)R[0], R[1]);
        return;
    case 5: {                                    /* OS_CLI */
        char s[1100]; int l = cstr(R[0], s, 1);
        ev("OS_CLI", "s", s, l);
        if (!strncasecmp(s, "QUIT", 4)) finish("OS_CLI QUIT");
        return;
    }
    case 0x11: finish("OS_Exit");
    case 0x2B: finish("OS_GenerateError");
    case 0x42: R[0] = rg_interactive ? (uint32_t)((display_ticks_ns() - rg_t0_ns) / CS_NS)   /* OS_ReadMonotonicTime */
                                     : (uint32_t)(icount / TICK_STEPS); return;
    }
    if (b >= 0xD0 && b <= 0xE8) { convert(b); return; }
    if (files_swi(b) || irq_swi(b) || sound_swi(b) || modules_swi(b, x != 0)) return;
    char name[48]; sprintf(name, "unhandled_SWI_%x", n);
    ev(name, "iii", R[0], R[1], R[2]);
    if (!x) finish("unhandled SWI");
    VF = 1;
}
