/* Game controllers as the keyboard.
 *
 * Each control of a controller (SDL's gamepads: an Xbox or PlayStation
 * pad, ...) holds a RISC OS key down while it is pressed, so a game that
 * reads the keyboard - INKEY, OS_Byte 121, key transition events, the
 * keyboard buffer - is played with it unchanged. The controls:
 *
 *     up down left right       the D-pad, and the left stick past half way
 *     a b x y                  the face buttons (Xbox names)
 *     lb rb lt rt              shoulders and triggers
 *     back start guide ls rs   menu buttons and stick clicks
 *
 * A mapping is "control=key,control=key,...": libroger's default (the
 * cursor keys, a Space, b and start Return, back Escape), then the game's
 * (roger_config.pad_map), then --pad=. A key is named as SDL names it -
 * "Z", "Return", "Space", "'", "/", "Up", "Left Shift" - or "Shift",
 * "Ctrl", "Alt", or inkeyN (INKEY -N); "none" leaves a control unmapped.
 * A second controller uses roger_config.pad2_map, or --pad2=, if either is
 * given, else the first one's mapping (two players on one keyboard).
 * Pressing a control also puts the key's character in the keyboard buffer
 * (Escape: the Escape condition), as the key itself would.
 *
 * Headless runs have no controllers; --script's pad:control[:ms] and
 * pad2:... press one, deterministically.
 *
 * Joysticks: a program that reads a joystick interface
 * (roger_config.joystick) gets the controllers as joysticks 0 and 1 when
 * one is connected at start (or --joystick): the D-pad and the left stick
 * are its directions, a and b its buttons, and those controls no longer
 * hold keys. Answered here, before any module that would read the
 * interface's hardware:
 *   ROGER_JOY_ACORN  Joystick_Read (&43F40; R0 the joystick): R0 bits 0-7
 *                    y, 8-15 x (signed: up and right positive), 16-23 the
 *                    buttons (bit 16 a, 17 b). A digital joystick's -64, 0
 *                    or 64, as the A3010's ports give - the value games
 *                    test against - or with ROGER_JOY_ANALOGUE the stick's
 *                    position (-127..127; the D-pad gives 127).
 *                    Joystick_CalibrateTopRight and BottomLeft do nothing.
 *   ROGER_JOY_KM     Keith Martin's interface (The Fourth Dimension's
 *                    games): Joy_Read0 and Joy_Read1 (&CFFC0, &CFFC1), the
 *                    port's five lines, high when pressed: KM_* below; and
 *                    the port itself, the byte at &33A0004 (joystick 0). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL3/SDL.h>
#include "internal.h"

uint8_t pad_down[128];                    /* controls holding each internal key */
int rg_pads = 1;                          /* use the host's controllers (--pad=off: no) */

static const char *const names[NPAD_CONTROLS] = {
    "up", "down", "left", "right", "a", "b", "x", "y", "lb", "rb", "lt", "rt",
    "back", "start", "guide", "ls", "rs",
};
static struct { int key, ch; } map[2][NPAD_CONTROLS];   /* internal key (-1 none), its character */
static uint8_t held[2][NPAD_CONTROLS];                  /* by source: PAD_BUTTON, PAD_STICK, PAD_SCRIPT */
static int joystick_control(int c);

int pad_control(const char *name, int len)
{
    for (int c = 0; c < NPAD_CONTROLS; c++)
        if ((int)strlen(names[c]) == len && !strncasecmp(names[c], name, len)) return c;
    return -1;
}

/* a key's internal number (-1: none) and the character it types (-1) */
static int key_named(const char *n, int *ch)
{
    *ch = -1;
    if (!strcasecmp(n, "none")) return -1;
    if (!strncasecmp(n, "inkey", 5)) { int i = atoi(n + 5); return i >= 1 && i <= 128 ? i - 1 : -2; }
    if (!strcasecmp(n, "Shift")) return 0;
    if (!strcasecmp(n, "Ctrl")) return 1;
    if (!strcasecmp(n, "Alt")) return 2;
    int k = host_internal_key(SDL_GetScancodeFromName(n));
    if (k < 0) return -2;
    if (strlen(n) == 1) *ch = (unsigned char)(n[0] >= 'A' && n[0] <= 'Z' ? n[0] + 32 : n[0]);
    else if (!strcasecmp(n, "Space")) *ch = ' ';
    else if (!strcasecmp(n, "Return")) *ch = 13;
    else if (!strcasecmp(n, "Tab")) *ch = 9;
    else if (!strcasecmp(n, "Backspace")) *ch = 8;
    else if (!strcasecmp(n, "Delete")) *ch = 127;
    return k;
}

/* apply a mapping string to controller p; 0 (with a message) if it's bad */
int pad_map(int p, const char *s)
{
    while (s && *s) {
        const char *e = strchr(s, ','), *eq = strchr(s, '=');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char key[64];
        int c = eq && eq < s + len ? pad_control(s, (int)(eq - s)) : -1;
        if (c < 0 || (size_t)(s + len - eq - 1) >= sizeof key) {
            fprintf(stderr, "pad mapping: no control in \"%.*s\" (controls: up down left right a b x y "
                    "lb rb lt rt back start guide ls rs)\n", (int)len, s);
            return 0;
        }
        snprintf(key, sizeof key, "%.*s", (int)(s + len - eq - 1), eq + 1);
        int ch, k = key_named(key, &ch);
        if (k == -2) { fprintf(stderr, "pad mapping: no key \"%s\"\n", key); return 0; }
        map[p][c].key = k; map[p][c].ch = ch;
        s += len + (e != NULL);
    }
    return 1;
}

int pad_setup(const char *opt1, const char *opt2)
{
    static const char *const dflt = "up=Up,down=Down,left=Left,right=Right,a=Space,b=Return,"
                                    "start=Return,back=Escape";
    for (int c = 0; c < NPAD_CONTROLS; c++) map[0][c].key = -1;
    if (!pad_map(0, dflt) || !pad_map(0, rg->pad_map) || !pad_map(0, opt1)) return 0;
    if (!rg->pad2_map && !opt2) memcpy(map[1], map[0], sizeof map[0]);
    else {
        for (int c = 0; c < NPAD_CONTROLS; c++) map[1][c].key = -1;
        if (!pad_map(1, dflt) || !pad_map(1, rg->pad2_map) || !pad_map(1, opt2)) return 0;
    }
    return 1;
}

static void km_port(void);

/* control c of controller p pressed or released by a source */
void pad_set(int p, int c, int source, int down)
{
    if (p < 0 || p > 1 || c < 0 || c >= NPAD_CONTROLS) return;
    int was = held[p][c] != 0;
    held[p][c] = down ? held[p][c] | source : held[p][c] & ~source;
    km_port();
    int now = held[p][c] != 0, k = joystick_control(c) ? -1 : map[p][c].key;
    if (now == was || k < 0) return;
    int both = k >= 3 && k <= 8 ? (k - 3) % 3 : -1;     /* left/right Shift, Ctrl, Alt: and either */
    if (now) {
        pad_down[k]++;
        if (both >= 0) pad_down[both]++;
        if (k == 112) escape_pressed = 1;                  /* one condition per press */
        else if (map[p][c].ch >= 0) key_buffer_put(map[p][c].ch);
    } else {
        if (pad_down[k]) pad_down[k]--;
        if (both >= 0 && pad_down[both]) pad_down[both]--;
    }
}

/* every control of controller p released (it was unplugged) */
void pad_release_all(int p)
{
    for (int c = 0; c < NPAD_CONTROLS; c++) pad_set(p, c, PAD_BUTTON | PAD_STICK | PAD_SCRIPT, 0);
}

/* ---------------------------------------------------------------- joysticks */
int rg_joystick;                          /* the interfaces answered (0: none) */
int rg_joystick_opt = -1;                 /* --joystick: 1, --joystick=off: 0, else -1 */

/* at start: the joystick on if the program reads one and a controller is
 * connected (have: how many), or --joystick */
void joystick_start(int have)
{
    int want = rg->joystick ? rg->joystick : ROGER_JOY_ACORN | ROGER_JOY_KM;
    rg_joystick = rg_joystick_opt == 1 ? want : rg_joystick_opt == 0 || !have ? 0 : rg->joystick;
    if (rg_joystick) fprintf(stderr, "joystick: the controllers (%s%s)\n",
                             rg_joystick & ROGER_JOY_ACORN ? "Joystick_Read" : "",
                             rg_joystick & ROGER_JOY_KM ? (rg_joystick & ROGER_JOY_ACORN ? ", Joy_Read" : "Joy_Read") : "");
}
static int axis[2][2];                    /* each controller's left stick, x and y */
/* Keith Martin's port lines, as Chocks Away reads them (its menus test
 * each against the key it stands for: X, Z, /, ') */
#define KM_RIGHT 0x01
#define KM_LEFT  0x02
#define KM_DOWN  0x04
#define KM_UP    0x08
#define KM_FIRE  0x10

void pad_axis(int p, int a, int v) { if (p >= 0 && p < 2 && a >= 0 && a < 2) axis[p][a] = v; }

static uint32_t km_lines(int p)
{
    return (held[p][PAD_UP] ? KM_UP : 0) | (held[p][PAD_DOWN] ? KM_DOWN : 0) |
           (held[p][PAD_LEFT] ? KM_LEFT : 0) | (held[p][PAD_RIGHT] ? KM_RIGHT : 0) |
           (held[p][PAD_A] || held[p][PAD_B] ? KM_FIRE : 0);
}

/* the interface's port, as a program that reads the hardware sees it (its
 * Joystick module does the same: Enter the Realm reads it itself): the
 * first joystick's lines in the byte at &33A0004 */
static void km_port(void)
{
    if ((rg_joystick & ROGER_JOY_KM) && MEM) MEM[0x33A0004] = (uint8_t)km_lines(0);
}

/* the joystick's controls: not keys while the joystick is on */
static int joystick_control(int c) { return rg_joystick && c <= PAD_B; }

/* an axis, -127..127 (positive: right, or up), from the stick or the D-pad */
static int joy_axis(int p, int a, int neg, int pos)
{
    int dig = held[p][pos] ? 1 : held[p][neg] ? -1 : 0;
    if (!(rg_joystick & ROGER_JOY_ANALOGUE)) return dig * 64;
    if ((held[p][pos] | held[p][neg]) & (PAD_BUTTON | PAD_SCRIPT)) return dig * 127;   /* the D-pad */
    int v = a ? -axis[p][a] : axis[p][a];                  /* (SDL's y is down) */
    v = v * 127 / 32767;
    return v > 127 ? 127 : v < -127 ? -127 : v > -12 && v < 12 ? 0 : v;   /* a small dead zone */
}

int joystick_swi(uint32_t b)
{
    if ((rg_joystick & ROGER_JOY_ACORN) && b >= 0x43F40 && b <= 0x43F42) {
        if (b == 0x43F40) {                                /* Joystick_Read */
            int p = R[0] & 0xFF;
            uint32_t v = 0;
            if (p <= 1) {
                v = (uint32_t)(joy_axis(p, 1, PAD_DOWN, PAD_UP) & 0xFF) |
                    (uint32_t)(joy_axis(p, 0, PAD_LEFT, PAD_RIGHT) & 0xFF) << 8 |
                    (held[p][PAD_A] ? 1u << 16 : 0) | (held[p][PAD_B] ? 1u << 17 : 0);
            }
            ev("Joystick_Read", "ii", R[0], v);
            R[0] = v;
        }
        return 1;                                          /* (calibration: nothing to do) */
    }
    if ((rg_joystick & ROGER_JOY_KM) && b >= 0xCFFC0 && b <= 0xCFFC3) {
        uint32_t v = 0;
        if (b <= 0xCFFC1) {                                /* Joy_Read0, Joy_Read1 */
            int p = b - 0xCFFC0;
            v = km_lines(p);
            ev("Joy_Read", "ii", p, v);
        }
        R[0] = v;
        return 1;
    }
    return 0;
}

int key_held(int k) { return k >= 0 && k < 128 && (key_down[k] || pad_down[k]); }
