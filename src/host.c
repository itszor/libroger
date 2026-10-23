/* The SDL3 front end: window, keyboard and mouse.
 *
 * The screen is shown at 4:3 as on an Archimedes monitor (a mode a module
 * defines at the shape its pixels give it: mode_aspect()), one texel per
 * pixel of the current mode (160, 320 or 640 by 256 on an Archimedes; any
 * size on a Risc PC), coloured through VIDC's palette (vidc_rgb()) or, at
 * 16 and 32 bits per pixel, directly (then through VIDC20's gamma tables). Keys are tracked by RISC OS internal key
 * number (INKEY -(n+1)) and typed characters go to a small key buffer.
 * With --crop only the rows the game's picture is on are shown (below).
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

uint8_t key_down[128];
int mouse_x = 640, mouse_y = 512, mouse_buttons;
int quit_requested, escape_pressed;

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;                        /* texture is exactly the game screen */
static int cur_shader = -1;                     /* display shader chain (shader.c), -1: none */
static char shader_capture[96];                 /* screenshot: also save the shaded frame */
static uint32_t *shader_pix;                    /* the frame as RGBA8 for the shaders */
static int kbuf[64], khead, ktail;
static SDL_DisplayMode fs_mode;                 /* exclusive fullscreen mode, if chosen */
static int fs_exclusive, fullscreen;
#define LH 768
static int lw = 1024;                           /* logical size: LH for the whole frame's height, */
static int lh = LH;                             /* the mode's shape; lh for the rows shown */
static float crop_y0;                           /* logical y of the first row shown (--crop) */
static int shown_rows = 256;                    /* the frame's rows shown */
/* The game's pointer hit point may sit some OS units below the host
 * pointer's (roger_config.pointer_y_offset). */
#define POINTER_Y_OFFSET (desktop_visible() ? 0 : rg->pointer_y_offset)   /* (not the desktop's) */

/* The host pointer maps absolutely onto the screen (OS units 1280x1024),
 * so a click or a touch lands exactly where the game's own software pointer
 * is drawn - the host pointer itself is hidden over the window. When the
 * game sets the pointer position (OS_Word 21,3), move the host pointer
 * there too, but only while it's over our window.
 *
 * With roger_config.relative_mouse, a game that keeps re-centring the
 * pointer (reading only its movement) gets the host mouse captured, in
 * relative mode, for as long as it does so. */
float rg_mouse_sens = 2.0f;                     /* OS units per host pixel in relative mode */
static uint64_t last_setpos_ns;
static int relative;
void display_set_mouse(int x, int y)
{
    mouse_x = x; mouse_y = y;
    last_setpos_ns = SDL_GetTicksNS();
    if (rg->relative_mouse) return;
    if (win && SDL_GetMouseFocus() == win) {
        float wx, wy;
        SDL_RenderCoordinatesToWindow(ren, (x + 0.5f) * lw / 1280,
                                      (1023 + POINTER_Y_OFFSET - y + 0.5f) * LH / 1024 - crop_y0, &wx, &wy);
        SDL_WarpMouseInWindow(win, wx, wy);
    }
}

/* --script tap: press the left button at screen pixel (px,py), as a host
 * mouse click or touch would, through the same event path. */
void display_inject_tap(int px, int py)
{
    float wx, wy;
    SDL_RenderCoordinatesToWindow(ren, (px + 0.5f) * lw / cur_mode()->width,
                                  (py + 0.5f) * LH / cur_mode()->height - crop_y0, &wx, &wy);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = SDL_GetWindowID(win); e.motion.x = wx; e.motion.y = wy;
    SDL_PushEvent(&e);
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.windowID = SDL_GetWindowID(win);
    e.button.button = SDL_BUTTON_LEFT; e.button.down = true; e.button.x = wx; e.button.y = wy;
    SDL_PushEvent(&e);
}

void key_buffer_put(int c)
{
    int n = (khead + 1) % 64;
    if (n != ktail) { kbuf[khead] = c; khead = n; }
}
void key_buffer_flush(void) { ktail = khead; }

int key_buffer_get(void)
{
    if (ktail == khead) return -1;
    int c = kbuf[ktail];
    ktail = (ktail + 1) % 64;
    return c;
}

/* SDL scancode -> RISC OS internal key number (INKEY -(n+1)), -1 if none */
static int internal_key(SDL_Scancode s);
int host_internal_key(int s) { return internal_key((SDL_Scancode)s); }

static int internal_key(SDL_Scancode s)
{
    static const struct { SDL_Scancode s; int k; } map[] = {
        {SDL_SCANCODE_LSHIFT, 3}, {SDL_SCANCODE_RSHIFT, 6}, {SDL_SCANCODE_LCTRL, 4},
        {SDL_SCANCODE_RCTRL, 7}, {SDL_SCANCODE_LALT, 5}, {SDL_SCANCODE_RALT, 8},
        {SDL_SCANCODE_Q, 16}, {SDL_SCANCODE_3, 17}, {SDL_SCANCODE_4, 18}, {SDL_SCANCODE_5, 19},
        {SDL_SCANCODE_F4, 20}, {SDL_SCANCODE_8, 21}, {SDL_SCANCODE_F7, 22}, {SDL_SCANCODE_MINUS, 23},
        {SDL_SCANCODE_LEFT, 25}, {SDL_SCANCODE_KP_6, 26}, {SDL_SCANCODE_KP_7, 27},
        {SDL_SCANCODE_F11, 28}, {SDL_SCANCODE_F12, 29}, {SDL_SCANCODE_F10, 30},
        {SDL_SCANCODE_SCROLLLOCK, 31}, {SDL_SCANCODE_PRINTSCREEN, 32}, {SDL_SCANCODE_W, 33},
        {SDL_SCANCODE_E, 34}, {SDL_SCANCODE_T, 35}, {SDL_SCANCODE_7, 36}, {SDL_SCANCODE_I, 37},
        {SDL_SCANCODE_9, 38}, {SDL_SCANCODE_0, 39}, {SDL_SCANCODE_DOWN, 41},
        {SDL_SCANCODE_KP_8, 42}, {SDL_SCANCODE_KP_9, 43}, {SDL_SCANCODE_PAUSE, 44},
        {SDL_SCANCODE_GRAVE, 45}, {SDL_SCANCODE_BACKSPACE, 47}, {SDL_SCANCODE_1, 48},
        {SDL_SCANCODE_2, 49}, {SDL_SCANCODE_D, 50}, {SDL_SCANCODE_R, 51}, {SDL_SCANCODE_6, 52},
        {SDL_SCANCODE_U, 53}, {SDL_SCANCODE_O, 54}, {SDL_SCANCODE_P, 55},
        {SDL_SCANCODE_LEFTBRACKET, 56}, {SDL_SCANCODE_UP, 57}, {SDL_SCANCODE_KP_PLUS, 58},
        {SDL_SCANCODE_KP_MINUS, 59}, {SDL_SCANCODE_KP_ENTER, 60}, {SDL_SCANCODE_INSERT, 61},
        {SDL_SCANCODE_HOME, 62}, {SDL_SCANCODE_PAGEUP, 63}, {SDL_SCANCODE_CAPSLOCK, 64},
        {SDL_SCANCODE_A, 65}, {SDL_SCANCODE_X, 66}, {SDL_SCANCODE_F, 67}, {SDL_SCANCODE_Y, 68},
        {SDL_SCANCODE_J, 69}, {SDL_SCANCODE_K, 70}, {SDL_SCANCODE_APOSTROPHE, 79},
        {SDL_SCANCODE_RETURN, 73}, {SDL_SCANCODE_KP_DIVIDE, 74}, {SDL_SCANCODE_KP_PERIOD, 76},
        {SDL_SCANCODE_NUMLOCKCLEAR, 77}, {SDL_SCANCODE_PAGEDOWN, 78}, {SDL_SCANCODE_S, 81},
        {SDL_SCANCODE_C, 82}, {SDL_SCANCODE_G, 83}, {SDL_SCANCODE_H, 84}, {SDL_SCANCODE_N, 85},
        {SDL_SCANCODE_L, 86}, {SDL_SCANCODE_SEMICOLON, 87}, {SDL_SCANCODE_RIGHTBRACKET, 88},
        {SDL_SCANCODE_DELETE, 89}, {SDL_SCANCODE_KP_MULTIPLY, 91}, {SDL_SCANCODE_EQUALS, 93},
        {SDL_SCANCODE_TAB, 96}, {SDL_SCANCODE_Z, 97}, {SDL_SCANCODE_SPACE, 98},
        {SDL_SCANCODE_V, 99}, {SDL_SCANCODE_B, 100}, {SDL_SCANCODE_M, 101},
        {SDL_SCANCODE_COMMA, 102}, {SDL_SCANCODE_PERIOD, 103}, {SDL_SCANCODE_SLASH, 104},
        {SDL_SCANCODE_END, 105}, {SDL_SCANCODE_KP_0, 106}, {SDL_SCANCODE_KP_1, 107},
        {SDL_SCANCODE_KP_3, 108}, {SDL_SCANCODE_ESCAPE, 112}, {SDL_SCANCODE_F1, 113},
        {SDL_SCANCODE_F2, 114}, {SDL_SCANCODE_F3, 115}, {SDL_SCANCODE_F5, 116},
        {SDL_SCANCODE_F6, 117}, {SDL_SCANCODE_F8, 118}, {SDL_SCANCODE_F9, 119},
        {SDL_SCANCODE_BACKSLASH, 120}, {SDL_SCANCODE_RIGHT, 121}, {SDL_SCANCODE_KP_4, 122},
        {SDL_SCANCODE_KP_5, 123}, {SDL_SCANCODE_KP_2, 124},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].s == s) return map[i].k;
    return -1;
}

/* ---------------------------------------------------------------- game controllers
 * The first two controllers connected are pads 0 and 1 (pad.c maps their
 * controls to keys). The left stick and the triggers count as pressed
 * past half way, released below a quarter. */
static SDL_JoystickID pad_id[2];

static int pad_of(SDL_JoystickID id)
{
    for (int p = 0; p < 2; p++) if (pad_id[p] == id) return p;
    return -1;
}

static void gamepad_event(const SDL_Event *e)
{
    static const int button[SDL_GAMEPAD_BUTTON_COUNT] = {
        [SDL_GAMEPAD_BUTTON_SOUTH] = PAD_A + 1, [SDL_GAMEPAD_BUTTON_EAST] = PAD_B + 1,
        [SDL_GAMEPAD_BUTTON_WEST] = PAD_X + 1, [SDL_GAMEPAD_BUTTON_NORTH] = PAD_Y + 1,
        [SDL_GAMEPAD_BUTTON_BACK] = PAD_BACK + 1, [SDL_GAMEPAD_BUTTON_GUIDE] = PAD_GUIDE + 1,
        [SDL_GAMEPAD_BUTTON_START] = PAD_START + 1, [SDL_GAMEPAD_BUTTON_LEFT_STICK] = PAD_LS + 1,
        [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = PAD_RS + 1, [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = PAD_LB + 1,
        [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = PAD_RB + 1, [SDL_GAMEPAD_BUTTON_DPAD_UP] = PAD_UP + 1,
        [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = PAD_DOWN + 1, [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = PAD_LEFT + 1,
        [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = PAD_RIGHT + 1,
    };
    int p;
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        for (p = 0; p < 2 && pad_id[p]; p++) ;
        if (p < 2 && SDL_OpenGamepad(e->gdevice.which)) {
            pad_id[p] = e->gdevice.which;
            SDL_Log("controller %d: %s", p + 1, SDL_GetGamepadNameForID(e->gdevice.which));
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if ((p = pad_of(e->gdevice.which)) >= 0) {
            pad_release_all(p);
            SDL_CloseGamepad(SDL_GetGamepadFromID(pad_id[p]));
            pad_id[p] = 0;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if ((p = pad_of(e->gbutton.which)) >= 0 && e->gbutton.button < SDL_GAMEPAD_BUTTON_COUNT &&
            button[e->gbutton.button])
            pad_set(p, button[e->gbutton.button] - 1, PAD_BUTTON, e->gbutton.down);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if ((p = pad_of(e->gaxis.which)) < 0) break;
        int v = e->gaxis.value;
        #define AXIS(neg, pos) do { \
            if (v > 16384) pad_set(p, pos, PAD_STICK, 1); else if (v < 8192) pad_set(p, pos, PAD_STICK, 0); \
            if ((neg) >= 0) { if (v < -16384) pad_set(p, neg, PAD_STICK, 1); else if (v > -8192) pad_set(p, neg, PAD_STICK, 0); } \
        } while (0)
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: pad_axis(p, 0, v); AXIS(PAD_LEFT, PAD_RIGHT); break;
        case SDL_GAMEPAD_AXIS_LEFTY: pad_axis(p, 1, v); AXIS(PAD_UP, PAD_DOWN); break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: AXIS(-1, PAD_LT); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: AXIS(-1, PAD_RT); break;
        }
        #undef AXIS
        break;
    }
    }
}

/* Fullscreen keeps the 4:3 picture: the logical presentation letterboxes it
   (bars at the sides on a widescreen monitor).  Exclusive mode switches the
   monitor to a real display mode; desktop mode just scales. */
static void apply_fullscreen(void)
{
    if (fullscreen && fs_exclusive) SDL_SetWindowFullscreenMode(win, &fs_mode);
    else SDL_SetWindowFullscreenMode(win, NULL);            /* borderless desktop */
    SDL_SetWindowFullscreen(win, fullscreen);
    SDL_SyncWindow(win);
}

void display_toggle_fullscreen(void)
{
    fullscreen = !fullscreen;
    apply_fullscreen();
}

void display_list_modes(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return; }
    int n = 0;
    SDL_DisplayID d = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(d);
    if (cur) printf("current: %dx%d @ %.2fHz (driver %s)\n", cur->w, cur->h, cur->refresh_rate,
                    SDL_GetCurrentVideoDriver());
    SDL_DisplayMode **m = SDL_GetFullscreenDisplayModes(d, &n);
    for (int i = 0; i < n; i++)
        printf("  %dx%d @ %.2fHz%s\n", m[i]->w, m[i]->h, m[i]->refresh_rate,
               m[i]->pixel_density != 1.0f ? " (high density)" : "");
    SDL_free(m);
    SDL_Quit();
}

/* spec: NULL windowed, "" desktop fullscreen, "WxH[@Hz]" exclusive mode */
/* Display shaders (shader.c: RetroArch slang presets on SDL_GPU, through
 * SDL 3.4's GPU renderer): F10 / Shift+F10 cycle through them, starting
 * from none (each game pixel a sharp rectangle). Without a GPU renderer
 * with SPIR-V (Vulkan) there are none. */
static void set_shader(int i)
{
    int n = shader_count();
    if (!n) { if (i >= 0) SDL_Log("display shaders unavailable (no GPU renderer with SPIR-V)"); return; }
    if (i < -1) i = n - 1;
    if (i >= n) i = -1;
    if (!shader_select(i)) { SDL_Log("shader %s failed; none", shader_name(i)); i = -1; shader_select(-1); }
    cur_shader = i;
    char title[160];
    const char *t = rg->title ? rg->title : "libroger";
    if (i >= 0) snprintf(title, sizeof title, "%s [%s]", t, shader_name(i));
    else snprintf(title, sizeof title, "%s", t);
    SDL_SetWindowTitle(win, title);
    SDL_Log("shader: %s", i >= 0 ? shader_name(i) : "none");
}

void display_cycle_shader(int dir) { set_shader(cur_shader + dir); }

int display_init(int scale, const char *fs, const char *shader)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 0;
    }
    win = rg_desktop ? SDL_CreateWindow(rg->title ? rg->title : "libroger", rg_desktop_w, rg_desktop_h, SDL_WINDOW_RESIZABLE)
                     : SDL_CreateWindow(rg->title ? rg->title : "libroger", 256 * scale, 192 * scale, SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Log("window: %s", SDL_GetError()); return 0; }
    if (SDL_GetModState() & SDL_KMOD_CAPS) kbd_status &= ~0x10;   /* Caps Lock as the host has it */
    if (rg_pads && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) SDL_Log("game controllers: %s", SDL_GetError());
    joystick_start(rg_pads && SDL_HasGamepad());
    ren = SDL_CreateRenderer(win, SDL_GPU_RENDERER);
    if (ren && !shader_init(ren)) SDL_Log("display shaders unavailable (no SPIR-V)");
    if (!ren) ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { SDL_Log("renderer: %s", SDL_GetError()); return 0; }
    SDL_SetRenderLogicalPresentation(ren, lw, LH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_HideCursor();                            /* the game draws its own pointer */
    if (fs) {
        int w = 0, h = 0;
        float hz = 0;
        fullscreen = 1;
        if (*fs && sscanf(fs, "%dx%d@%f", &w, &h, &hz) >= 2) {
            if (SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(win), w, h, hz, false, &fs_mode)) {
                fs_exclusive = 1;
                SDL_Log("fullscreen mode %dx%d @ %.2fHz", fs_mode.w, fs_mode.h, fs_mode.refresh_rate);
            } else SDL_Log("no display mode near %s (%s); using desktop fullscreen", fs, SDL_GetError());
        }
        apply_fullscreen();
    }
    if (shader && *shader) {
        int i = shader_find(shader);
        if (i < 0 && SDL_strcasecmp(shader, "none") && SDL_strcasecmp(shader, "nearest"))
            SDL_Log("no shader %s (--list-shaders)", shader);
        set_shader(i);
    }
    return 1;
}

/* Pixel values -> host colours, for each of the frame's palette regions
 * (video_palette_region(): above the palette split and below it, or with
 * roger_config.raster_palette one for each timer interrupt down the
 * frame), rebuilt for each frame. */
#define MAXREGIONS 513
static uint32_t pals[MAXREGIONS][256];
static uint32_t apply_gamma(uint32_t c)
{
    if (!gamma_set) return c;
    return (uint32_t)gamma_tab[0][c >> 16 & 0xFF] << 16 | (uint32_t)gamma_tab[1][c >> 8 & 0xFF] << 8 | gamma_tab[2][c & 0xFF];
}
static void update_palette(void)
{
    if (cur_mode()->bpp > 8) return;
    int n = 1 << cur_mode()->bpp, regions = video_palette_regions();
    if (regions > MAXREGIONS) regions = MAXREGIONS;
    for (int r = 0; r < regions; r++)
        for (int c = 0; c < n; c++) pals[r][c] = apply_gamma(vidc_rgb_region(r, (uint8_t)c));
}

/* one line of the frame (mode pixels) as host colours */
static void convert_line(const uint8_t *pix, int y, uint32_t *row)
{
    const struct rg_mode *m = cur_mode();
    int region = video_palette_region(y);
    const uint32_t *pal = pals[region < MAXREGIONS ? region : MAXREGIONS - 1];
    const uint8_t *src = pix + (size_t)y * (m->width * m->bpp / 8);
    int bpp = m->bpp;
    if (bpp == 32) {                              /* &00BBGGRR */
        for (int x = 0; x < m->width; x++, src += 4)
            row[x] = apply_gamma((uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2]);
        return;
    }
    if (bpp == 16) {                              /* 5 bits each, red lowest */
        for (int x = 0; x < m->width; x++, src += 2) {
            int v = src[0] | src[1] << 8, r = v & 31, g = v >> 5 & 31, b = v >> 10 & 31;
            row[x] = apply_gamma((uint32_t)(r << 3 | r >> 2) << 16 | (uint32_t)(g << 3 | g >> 2) << 8 | (b << 3 | b >> 2));
        }
        return;
    }
    int per = 8 / bpp, mask = (1 << bpp) - 1;
    for (int x = 0; x < m->width; x++)
        row[x] = pal[bpp == 8 ? src[x] : (src[x / per] >> ((x % per) * bpp)) & mask];
}

/* a frame into the texture, which is made its size */
static int upload(const uint32_t *rgb, int width, int height)
{
    uint32_t *out;
    int pitch;
    if (width != tex_w || height != tex_h) {     /* one texel per pixel */
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        tex_w = width; tex_h = height;
    }
    if (!SDL_LockTexture(tex, NULL, (void **)&out, &pitch)) return 0;
    for (int y = 0; y < height; y++) memcpy((uint8_t *)out + y * pitch, rgb + (size_t)y * width, (size_t)width * 4);
    SDL_UnlockTexture(tex);
    return 1;
}

/* The desktop is shown as it is, pixel for pixel at the top left of the
 * window (it's made the window's size: display_output_size()), never
 * scaled or through a shader */
static int desk;                                  /* the desktop is showing */
void display_output_size(int *w, int *h)
{
    if (!SDL_GetRenderOutputSize(ren, w, h)) SDL_GetWindowSizeInPixels(win, w, h);
}
void display_present_desktop(const uint32_t *rgb, int width, int height)
{
    if (!desk) {
        desk = 1;
        SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    }
    if (!upload(rgb, width, height)) return;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_FRect d = {0, 0, (float)width, (float)height};
    SDL_RenderTexture(ren, tex, NULL, &d);
    SDL_RenderPresent(ren);
}

int display_fullscreen(void) { return fullscreen; }
void display_set_fullscreen(int on)
{
    if (!win || !on == !fullscreen) return;
    fullscreen = on;
    apply_fullscreen();
}

/* --crop: of a frame the game shows in only some of its rows (Lemmings'
 * 208 of 256 in a level, with black above and below), only those rows are
 * shown, at the same pixel shape, so that on a widescreen monitor the
 * picture is larger and letterboxed less. The rows are cut from the frame
 * before it is drawn or goes through a display shader, which still sees a
 * texel a pixel. At least enough rows to fill the monitor's shape are shown
 * (on a 16:9 one 192 of 256 lines of 4:3), centred on the rows asked for
 * where they can be - uneven bars are trimmed alike - so that what the
 * monitor would show as bars anyway is the frame's own.
 *
 * The rows are fixed, never measured from the frames: --crop=FIRST-LAST,
 * or the game's own settings, <program>.ini (--crop=FILE: another), whose
 * [crop] section gives them to start with (rows = FIRST-LAST) and from
 * when some code first runs (ADDR = FIRST-LAST, ADDR in hex: an execution
 * gate, cpu.c) - a splash screen's rows, say, then the game's for good
 * once its own code starts. */
static int crop_on, crop_first = -1, crop_last;   /* the rows asked for (crop_first -1: all) */
static int crop_top, crop_rows;                   /* the rows shown (crop_rows 0: all) */
#define MAXCROPGATES 32
static struct { uint32_t a; int first, last; } crop_gates[MAXCROPGATES];
static int ncrop_gates;

static void crop_gate(uint32_t a)
{
    for (int k = 0; k < ncrop_gates; k++)
        if (crop_gates[k].a == a) {
            crop_first = crop_gates[k].first; crop_last = crop_gates[k].last;
            if (crop_first < 0) SDL_Log("crop: from &%X, all rows", a);
            else SDL_Log("crop: from &%X, rows %d-%d", a, crop_first, crop_last);
        }
}

/* "FIRST-LAST", or "all" (first -1); 0 if it's neither */
static int crop_spec(const char *s, int *first, int *last)
{
    char c;
    if (!SDL_strcasecmp(s, "all")) { *first = -1; *last = 0; return 1; }
    return sscanf(s, "%d-%d%c", first, last, &c) == 2 && *first >= 0 && *last >= *first;
}

/* the [crop] section of an .ini file; 0 if it has lines it can't take */
static int crop_ini(const char *path, FILE *f)
{
    char line[256];
    int n = 0, ok = 1, in = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        line[strcspn(line, ";#\r\n")] = 0;              /* (comments) */
        char key[64], val[64], *p = line;
        int first, last;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        if (*p == '[') { in = !SDL_strncasecmp(p, "[crop]", 6); continue; }
        if (!in) continue;
        if (sscanf(p, "%63[^= \t] = %63s", key, val) == 2 && crop_spec(val, &first, &last)) {
            if (!SDL_strcasecmp(key, "rows")) { crop_first = first; crop_last = last; continue; }
            char *e;
            uint32_t a = (uint32_t)strtoul(key + (key[0] == '&'), &e, 16);
            if (!*e && e > key + (key[0] == '&') && ncrop_gates < MAXCROPGATES) {
                crop_gates[ncrop_gates].a = a; crop_gates[ncrop_gates].first = first;
                crop_gates[ncrop_gates++].last = last;
                continue;
            }
        }
        fprintf(stderr, "%s:%d: not a crop setting (rows = FIRST-LAST, ADDR = FIRST-LAST): %s\n", path, n, p);
        ok = 0;
    }
    return ok;
}

int display_set_crop(const char *spec, const char *program)
{
    char path[2048];
    crop_on = 1;
    if (spec && crop_spec(spec, &crop_first, &crop_last)) return 1;
    snprintf(path, sizeof path, "%s%s", spec ? spec : program, spec ? "" : ".ini");
    FILE *f = fopen(path, "r");
    if (!f) {
        if (spec) { perror(path); return 0; }
        fprintf(stderr, "--crop: no %s (its [crop] rows), so nothing is cropped\n", path);
        return 1;
    }
    int ok = crop_ini(path, f);
    fclose(f);
    for (int k = 0; k < ncrop_gates; k++)
        if (!cpu_gate(crop_gates[k].a, crop_gate))
            fprintf(stderr, "%s: can't watch for &%X\n", path, crop_gates[k].a);
    return ok;
}

/* the shape (width / height) of the screen the window is shown on */
static float screen_shape(void)
{
    if (fs_exclusive) return (float)fs_mode.w / fs_mode.h;
    const SDL_DisplayMode *d = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(win));
    return d && d->h ? (float)d->w / d->h : 16.0f / 9;
}

/* the rows to show of a frame height rows high, at the shape aspect for
 * all of them: those asked for, and as few more as fill the screen */
static void crop_frame(int height, float aspect)
{
    int first = crop_first, last = crop_last < height ? crop_last : height - 1;
    if (first < 0 || first > last) { crop_rows = 0; return; }
    int n = last - first + 1, fill = (int)(height * aspect / screen_shape() + 0.5f);
    int rows = n > fill ? n : fill > height ? height : fill;
    int top = first - (rows - n) / 2;
    crop_top = top < 0 ? 0 : top > height - rows ? height - rows : top;
    crop_rows = rows;
}

/* Show a frame of host colours (&RRGGBB) at the shape given (display
 * width / height), letterboxed: the game's screen */
void display_present_rgb(const uint32_t *rgb, int width, int height, float aspect)
{
    int full = height;
    crop_y0 = 0;
    if (crop_on) {
        crop_frame(height, aspect);
        if (crop_rows) {
            rgb += (size_t)crop_top * width;
            height = crop_rows;
            crop_y0 = (float)LH * crop_top / full;
        }
    }
    shown_rows = height;
    int want = (int)(LH * aspect + 0.5f), want_h = (int)((float)LH * height / full + 0.5f);
    if (desk) { desk = 0; lw = 0; }              /* back from the desktop */
    if (want != lw || want_h != lh) {            /* the shape changed */
        lw = want; lh = want_h;
        SDL_SetRenderLogicalPresentation(ren, lw, lh, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        int ww, wh;
        if (!fullscreen && SDL_GetWindowSize(win, &ww, &wh))
            SDL_SetWindowSize(win, wh * lw / lh, wh);
    }
    if (!upload(rgb, width, height)) return;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_Texture *shaded = NULL;
    if (cur_shader >= 0) {
        /* the frame again as RGBA8 (R first in memory) for the shader's upload */
        static int shader_w, shader_h;
        if (shader_w != width || shader_h != height) {
            SDL_free(shader_pix); shader_pix = SDL_malloc((size_t)width * height * 4);
            shader_w = width; shader_h = height;
        }
        for (size_t k = 0; shader_pix && k < (size_t)width * height; k++) {
            uint32_t c = rgb[k];
            shader_pix[k] = 0xFF000000u | (c & 0xFF00u) | (c >> 16 & 0xFFu) | (c & 0xFFu) << 16;
        }
        SDL_FRect vp;
        SDL_GetRenderLogicalPresentationRect(ren, &vp);
        if (shader_pix && vp.w >= 1 && vp.h >= 1)
            shaded = shader_run(shader_pix, width, height, (int)(vp.w + 0.5f), (int)(vp.h + 0.5f),
                                aspect * full / height);
    }
    SDL_RenderTexture(ren, shaded ? shaded : tex, NULL, NULL);
    if (shader_capture[0]) {
        SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
        if (shot) { SDL_SaveBMP(shot, shader_capture); SDL_DestroySurface(shot); }
        shader_capture[0] = 0;
    }
    SDL_RenderPresent(ren);
}

void display_present(const uint8_t *pix)
{
    static uint32_t *frame;
    static size_t frame_n;
    int width = cur_mode()->width, height = cur_mode()->height;
    if ((size_t)width * height > frame_n) {
        frame_n = (size_t)width * height;
        SDL_free(frame); frame = SDL_malloc(frame_n * 4);
        if (!frame) { frame_n = 0; return; }
    }
    update_palette();
    for (int y = 0; y < height; y++) convert_line(pix, y, frame + (size_t)y * width);
    display_present_rgb(frame, width, height, mode_aspect(cur_mode()));
}

static int ascii_for(SDL_Scancode s, int shift)
{
    SDL_Keycode k = SDL_GetKeyFromScancode(s, shift ? SDL_KMOD_SHIFT : SDL_KMOD_NONE, false);
    if (k == SDLK_RETURN) return 13;
    if (k == SDLK_BACKSPACE || k == SDLK_DELETE) return 127;
    if (k == SDLK_TAB) return 9;
    if (k >= 32 && k < 127) return (int)k;
    return -1;
}

/* The host pointer takes the shape the program gave the OS pointer
 * (OS_Word 21,0, OS_SpriteOp 36) in its colours, a pointer pixel the size
 * of a screen pixel as shown (the desktop's: a host pixel); the system's
 * own pointer for a shape never defined (the Wimp's). */
static void update_cursor(void)
{
    static SDL_Cursor *cursor;
    static int shown_shape = -1, shown_gen = -1;
    static float shown_sx, shown_sy;
    if (!win) return;
    const struct pointer_def *d = &pointer_defs[pointer_shape & 15];
    float sx = 1, sy = 1, density = SDL_GetWindowPixelDensity(win);
    SDL_FRect vp;
    if (!desk && SDL_GetRenderLogicalPresentationRect(ren, &vp) && vp.w >= 1 && vp.h >= 1) {
        sx = vp.w / cur_mode()->width; sy = vp.h / shown_rows;
    }
    if (density > 0) { sx /= density; sy /= density; }
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    if (pointer_shape == shown_shape && pointer_gen == shown_gen && sx == shown_sx && sy == shown_sy) return;
    shown_shape = pointer_shape; shown_gen = pointer_gen; shown_sx = sx; shown_sy = sy;
    SDL_Cursor *old = cursor;
    cursor = NULL;
    int w = (int)(d->w * sx + 0.5f), h = (int)(d->h * sy + 0.5f);
    SDL_Surface *s = d->w && d->h ? SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (s) {
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
            for (int x = 0; x < w; x++) {
                int p = d->pix[(int)(y / sy) * 32 + (int)(x / sx)];
                row[x] = p ? 0xFF000000u | pointer_rgb[p] : 0;
            }
        }
        int hx = (int)((d->ax + 0.5f) * sx), hy = (int)((d->ay + 0.5f) * sy);
        cursor = SDL_CreateColorCursor(s, hx < w ? hx : w - 1, hy < h ? hy : h - 1);
        SDL_DestroySurface(s);
    }
    SDL_SetCursor(cursor ? cursor : SDL_GetDefaultCursor());
    if (old) SDL_DestroyCursor(old);
}

void display_poll(void)
{
    SDL_Event e;
    if (rg->relative_mouse) {
        int want = last_setpos_ns && SDL_GetTicksNS() - last_setpos_ns < 500000000ull && !desktop_visible();
        if (want != relative) {
            relative = want;
            SDL_SetWindowRelativeMouseMode(win, relative);
        }
    }
    /* A game that turns the OS pointer on (OS_Byte 106) and doesn't draw its
     * own sees the host pointer in its place, except while captured. */
    static int cursor_shown;
    int show = (pointer_shape || desktop_visible()) && !relative;
    if (show != cursor_shown) {
        cursor_shown = show;
        if (show) SDL_ShowCursor(); else SDL_HideCursor();
    }
    if (show) update_cursor();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit_requested = 1;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            gamepad_event(&e);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int down = e.type == SDL_EVENT_KEY_DOWN;
            /* the runtime's own keys - F10 / Shift+F10 shaders, F11 (or
             * Alt+Return) fullscreen, F12 screenshot - Ctrl+ those with
             * roger_config.ctrl_meta_keys, for games that use F10-F12 */
            SDL_Scancode sc = e.key.scancode;
            int meta_mod = rg->ctrl_meta_keys ? (e.key.mod & SDL_KMOD_CTRL) != 0 : 1;
            if (meta_mod && (sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12)) {
                if (down && !e.key.repeat) {
                    if (sc == SDL_SCANCODE_F10) display_cycle_shader((e.key.mod & SDL_KMOD_SHIFT) ? -1 : 1);
                    else if (sc == SDL_SCANCODE_F11) display_toggle_fullscreen();
                    else screenshot_requested = 1;
                }
                break;                                      /* not passed to the game */
            }
            if (sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                if (down && !e.key.repeat) display_toggle_fullscreen();
                break;
            }
            int k = internal_key(e.key.scancode);
            if (k >= 0) key_down[k] = down;
            if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_CAPSLOCK) kbd_status ^= 0x10;
            if (e.key.scancode == SDL_SCANCODE_LSHIFT || e.key.scancode == SDL_SCANCODE_RSHIFT)
                key_down[0] = down;
            if (e.key.scancode == SDL_SCANCODE_LCTRL || e.key.scancode == SDL_SCANCODE_RCTRL)
                key_down[1] = down;
            if (e.key.scancode == SDL_SCANCODE_LALT || e.key.scancode == SDL_SCANCODE_RALT)
                key_down[2] = down;
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                if (down && !e.key.repeat) escape_pressed = 1;   /* one condition per press */
            }
            else if (down && !e.key.repeat) {
                int c = ascii_for(e.key.scancode, (e.key.mod & SDL_KMOD_SHIFT) != 0);
                if (c >= 0) key_buffer_put(c);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (relative) {
                if (e.type == SDL_EVENT_MOUSE_MOTION) {
                    static float fx, fy;            /* (what's left of a unit, kept) */
                    fx += e.motion.xrel * rg_mouse_sens; fy -= e.motion.yrel * rg_mouse_sens;
                    mouse_x += (int)fx; mouse_y += (int)fy;
                    fx -= (int)fx; fy -= (int)fy;
                }
            } else {
                SDL_ConvertEventToRenderCoordinates(ren, &e);
                float x = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
                float y = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
                if (desk) {                             /* the desktop: a pixel is 2x2 OS units */
                    mouse_x = (int)x * 2;
                    mouse_y = (tex_h - 1 - (int)y) * 2;
                } else {                                /* (within the rows shown) */
                    y = y < 0 ? 0 : y > lh - 0.01f ? lh - 0.01f : y;
                    mouse_x = (int)(x * 1280 / lw);
                    mouse_y = 1023 + POINTER_Y_OFFSET - (int)((y + crop_y0) * 1024 / LH);
                }
            }
            int mw = desk ? tex_w * 2 : 1280, mh = desk ? tex_h * 2 : 1024;
            mouse_x = mouse_x < 0 ? 0 : mouse_x > mw - 1 ? mw - 1 : mouse_x;
            mouse_y = mouse_y < 0 ? 0 : mouse_y > mh - 1 ? mh - 1 : mouse_y;
            if (e.type != SDL_EVENT_MOUSE_MOTION) {
                int bit = e.button.button == SDL_BUTTON_LEFT ? 4 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 1;
                int kn = bit == 4 ? 9 : bit == 2 ? 10 : 11;
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_buttons |= bit; else mouse_buttons &= ~bit;
                key_down[kn] = (mouse_buttons & bit) != 0;
            }
            break;
        }
        default:
            break;
        }
    }
}

/* save the displayed screen as BMP (F12, or --script shot) */
void display_screenshot(const uint8_t *pix, const char *path)
{
    SDL_Surface *s = SDL_CreateSurface(cur_mode()->width, cur_mode()->height, SDL_PIXELFORMAT_XRGB8888);
    if (!s) return;
    update_palette();
    for (int y = 0; y < cur_mode()->height; y++) convert_line(pix, y, (uint32_t *)((uint8_t *)s->pixels + y * s->pitch));
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    if (cur_shader >= 0)                         /* the shaded frame is saved by the next present */
        snprintf(shader_capture, sizeof shader_capture, "%.*s-%s.bmp", (int)strcspn(path, "."), path,
                 shader_name(cur_shader));
}
int screenshot_requested;

void display_screenshot_rgb(const uint32_t *rgb, int w, int h, const char *path)
{
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
    if (!s) return;
    for (int y = 0; y < h; y++) memcpy((uint8_t *)s->pixels + y * s->pitch, rgb + (size_t)y * w, (size_t)w * 4);
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    if (cur_shader >= 0)
        snprintf(shader_capture, sizeof shader_capture, "%.*s-%s.bmp", (int)strcspn(path, "."), path,
                 shader_name(cur_shader));
}

uint64_t display_ticks_ns(void) { return SDL_GetTicksNS(); }
void display_sleep_ns(uint64_t ns) { SDL_DelayNS(ns); }
