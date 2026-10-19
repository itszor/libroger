libroger - RISC OS Game Execution Runtime
=========================================

A runtime for statically recompiled 26-bit ARM (ARM2/ARM3) RISC OS
programs - in practice, Archimedes games. A code generator translates the
program's machine code to C, one function per routine; libroger supplies
everything around it: the CPU state the translated code works on, dispatch
between translated functions (with an ARM2 interpreter for anything not
translated), the parts of the machine games touch directly (MEMC video DMA,
VIDC palette and timing, IOC timers), the RISC OS calls they make, and an
SDL3 front end (window, keyboard, mouse, sound).

It contains no code from RISC OS or from any game: the OS behaviour is
reimplemented from its documented interfaces, and everything specific to
one game is supplied by that game's own build.

Building
--------

    make            libroger.a        (needs SDL3: pkg-config sdl3)
    make cov COV=<dir with cov.h>     libroger-cov.a, with coverage hooks

A game links its translated code and a small game.c with libroger.a and
`$(pkg-config --libs sdl3) -lm -lpthread`, compiling with
`-I<libroger>/include`.

Using it
--------

The translated code includes `roger_cpu.h` (the CPU state and the macros
the generator emits) and provides a `struct roger_image` per translated
image: its address range, entry points and a bitmap of code words. The game
fills in a `struct roger_config` (roger.h) and calls `roger_main()`:

    #include <roger.h>
    extern const struct roger_image recomp_image;
    static const struct roger_config config = {
        .title = "Some Game", .image_path = "../extracted/!Game/!RunImage",
        .image = &recomp_image, .appdir = "../extracted/!Game",
        .dir_prefixes = (const char *const[]){"<Game$Dir>.", NULL},
        .initial_mode = 9, .wimpslot = 640 * 1024,
    };
    int main(int argc, char **argv) { roger_main(argc, argv, &config); }

What the configuration covers (all but the image optional):

- the program and its translation, and where it starts if not at its load
  address (`entry`, an absolute file's execution address); other programs
  it may chain-load at their image's base (`programs`); images that copy
  themselves elsewhere and run from the copy (`relocations`, with code
  translated relative to `pc_bias`); relocatable modules to RMLoad first,
  or when the program asks for them (`modules`, `on_demand`: *RMLoad,
  *RMRun, OS_Module, and *RMKill again; modules never loaded together may
  share an address and a translation); loader programs !Run runs before it
  (`loader_programs`, interpreted; OS_Exit ends the loader, not the run)
- its files: the host directory standing in for the application directory
  and the RISC OS prefixes that mean it, system variables as !Run sets
  them (`variables`; `<Game$Dir>` in a filename or command expands) and the
  current directory it leaves (`directory`: *Dir)
- the machine as !Run left it: screen mode, WimpSlot, screen memory size and
  placement, initial stack; and whether it's an Archimedes or a Risc PC
  (`vidc20`)
- interrupts: a raw IRQ handler at &100 (`irq1v`) for games that bypass the
  kernel's device vectors
- input: pointer offset, relative mouse capture (and its sensitivity:
  `mouse_sens`, `--mouse-sens=N`), game controllers (`pad_map`, `pad2_map`:
  the keys their controls hold), the headless scenario
- translation gaps that are known and may be interpreted
- hooks: init, every frame, every centisecond, first look at SWIs and at
  dispatch, extra command-line arguments, the program returning to the OS
  (`exited`: what !Run does next), finish
- the desktop, for a program that is a Wimp task: `wimp_idle` stands in
  for the user when Wimp_Poll has nothing to return, queueing events
  (`roger_wimp_click()`, `roger_wimp_menu()`, `roger_wimp_quit()`); on
  the desktop (`--desktop`), directory viewers to open
  (`roger_filer_open()`) and what running an application or file from
  one does (`filer_run`)

Game code (hooks, or a loader written in C - e.g. a BASIC program
translated by hand) can call SWIs and * commands with `roger_swi()` and
`roger_cli()`, and read and set system variables.

Command line (any game): `--limit=N` (headless, deterministic),
`--script=...` (scripted input: keys, clicks, taps, screenshots - in
headless runs too, timed in emulated milliseconds, so a scenario can be
replayed and screenshotted deterministically),
`--desktop[=WxH]` and `--windowed-games` (below), `--trace-gaps`, `--full-os`, `--no-title`, `--scale=N`, `--mips=N` (the CPU's
speed in interactive runs, for games that don't pace themselves:
roger_config.mips; unthrottled by default),
`--mouse-sens=N`, `--pad=MAP`, `--pad2=MAP` and `--pad=off` (game
controllers, below), `--joystick` and `--joystick=off`, `--fullscreen[=WxH@Hz]`, `--shader=NAME` (display shader, see below;
`--crt` = `--shader=crt`), `--list-shaders`, `--no-sound`, `--wav=F`,
`--app=DIR`, `--save=DIR`, `--out=PREFIX`; `--help` lists them.

Determinism: headless runs (`--limit`) advance time by instruction count
(a centisecond per 20000, a frame per 40000: 2 MIPS, or the game's
roger_config.headless_mips, or --headless-mips=N) and write the event log,
VDU stream, screen memory and registers, so two builds of a game can be
compared byte for byte. `ROGER_INTERPRET=1` interprets everything, to
compare a translation's behaviour with the interpreter's.

The emulated machine
--------------------

    &0000000  page zero (&100: optional raw IRQ handler pointer)
    &0008000  the application, to the RAM limit (&8000 + WimpSlot)
    &17F0000  runtime workspace: sound DMA buffers, sound channel control
              blocks, the sound system's Level 1 block and tables,
              OS_GetEnv's strings, SVC and IRQ stacks, module private words
    &1800000  RMA: modules, then OS_Module claims from &1810000
    ........  screen memory, ending at &2000000 by default
    &3200000  IOC (timer 1 latch and IRQ registers), &3400000 VIDC,
              &3600000 MEMC: writes are emulated

Implemented:

- the pointer: the OS's (OS_Byte 106) is shown as the host's pointer; a
  mode change turns it off, unless roger_config.desktop_pointer (a game
  started from the desktop relying on the Wimp's pointer staying on)
- overlays (roger_config.programs with overlay set): the image loaded
  last at an address is the one dispatched there, over an earlier overlay
  or the program; a gap in one is treated as the program's. A program is
  named by its leaf, or by its last components where two files share a
  leaf ("Data.CODE": Enter the Realm)
- CPU: processor modes with banked registers, TEQP mode changes, the
  ARM2 interpreter (gaps, self-modified instructions); self-modified code
  at run time: a compiled word the program stores to is interpreted as it
  now is until it holds its original value again
- interrupts: TickerV, EventV (vsync, mouse, keys; run in IRQ mode on the
  IRQ stack, as RISC OS does, so the interrupted code's R13 is left alone;
  a claimant can intercept with the return address on the stack), OS_ClaimDeviceVector (vsync,
  IOC timer 1, the sound DMA buffer interrupt), the raw handler at &100;
  OS_IntOff/IntOn (the I bit; interrupts wait); module SWIs run with
  interrupts enabled, as on RISC OS (one may wait for a handler; entered
  in SVC mode with R14's PSR bits saying so, as the kernel calls them);
  palette changes part way down the
  frame (timer 1 started at flyback, VIDC VDSR/VDER/VCR timing); VIDC's
  vertical display start and end (VDSR/VDER: the picture moved up or down,
  or cropped - Saloon Cars shakes the screen with them and HDSR/HDER every
  frame) and horizontal timing (HDSR/HDER: a longer or shifted line, as a program
  scrolling by a pixel or two at a time sets; HBSR/HBER: the border),
  shown in the standard mode's window
- fonts (src/fonts.c): the Font Manager for RISC OS 2's pixel fonts - a
  font directory along Font$Prefix or Font$Path with IntMetrics and
  x90y45 - at any size and resolution (the nearest larger bitmap size,
  scaled), anti-aliased into the screen's closest colours: Font_FindFont,
  LoseFont, SetFont, CurrentFont, SetFontColours, SetPalette, Paint (with
  its colour, font and move sequences), StringWidth, ConverttoOS and
  Converttopoints; no outline fonts
- video: the standard 50Hz graphics modes (0, 1, 2, 4, 5, 8, 9, 10, 12, 13,
  15), modes modules define (Service_ModeExtension: VIDC and workspace
  lists, e.g. an overscan mode, shown at their own size; asked about for
  roger_config.monitor_type, a TV monitor by default), screen banks
  (OS_Byte 112/113, returning the previous bank; OS_Word 22), MEMC Vinit/Vstart/Vend
  with wrap-around, VIDC palette including 256-colour modes, VDU and mode
  variables, VDU drivers (MODE, CLS/CLG, COLOUR/GCOL, VDU 19 palette,
  tints, windows, origin, PLOT lines/points/triangles/rectangles; VDU
  21/6; circles and ellipses, outline and filled; flood fill (PLOT
  &80-&8F, BASIC's FILL); text: VDU 4 in a text window, VDU 5 at the graphics cursor, VDU
  23 characters, cursor control), OS_SpriteOp on user areas and the
  system sprite area (*SLoad/*SMerge; load, merge, read area information,
  select, plot; *SChoose and PLOT &E8-&EF; *ScreenLoad and SpriteOp 3:
  a sprite file's first sprite to the screen, in its mode, with its
  palette), *Print and BBC font files
  (&FF7) run by name. OS_Byte 113 and OS_Word 22 set the same display
  start (the last one counts). A mode
  change resets the VIDC/MEMC display timing a program set itself.
  The character set is Acorn Mode 0's (see The character set, below) -
  not the ROM's
- a Risc PC (`vidc20`): VIDC20's modes - the multisync numbered ones
  (18-21, 25-28) and RISC OS 3.5 mode selectors, any size up to 2MB of
  VRAM at 1 to 32 bits per pixel, shown as they are (no MEMC or VIDC1
  raster effects) - OS_ScreenMode (select, read), gamma tables (PaletteV
  9, through OS_CallAVector), OS_Memory 8 (2MB of VRAM), and what
  programs test to tell one from an Archimedes: Wimp version 3.50 and the
  RISC OS 3.5 names for OS_SWINumberFromString
- files: OS_File (load, save, catalogue; along a path string or
  variable; directories), OS_Find, OS_GBPB (including
  directory listings, 9/10/12: the program's saved copies as well as the
  originals), OS_Args,
  OS_BGet/BPut on the host directory, catalogue information from `,xxx` /
  `,load-exec` name suffixes, writes redirected to a save directory (and
  read back from there)
- the floppy disc (roger_config.floppy, --floppy=), for programs that
  read it sector by sector, as copy protection does: FileCore_DiscOp and
  ADFS_DiscOp reads and verifies from an HxC .hfe image (its MFM tracks
  decoded with each sector's ID, size and CRC: odd sectors and CRC errors
  as on the disc; a bad sector's data is transferred before the error) or
  a flat 800K/1600K .adf, alternative disc records, ADFS_Retries;
  OS_FSControl 14 and 20 (ADFS's module base and FileCore instance);
  FileCore and ADFS as "ROM" modules at &3800000 that OS_Module 18 finds,
  whose SWI handlers come back to the same calls (src/disc.c)
- a RAM disc (roger_config.ramfs_size): RAM:$ as a temporary host
  directory, empty at start and removed at exit; OS_ReadRAMFsLimits,
  RamFS_FreeSpace
- sound: DMA at the configured rate with VIDC log samples and stereo
  positions; a program that drives the DMA itself (MEMC Sstart/SendN,
  the device 9 interrupt, the OS's buffers at &1F06000/&1F07000 as RISC
  OS 3.1 maps them - OS_ReadMemMapEntries finds that page, physical
  &78000 - and VIDC1's sound frequency and stereo image registers once it
  writes them); MEMC's control register (written directly or with
  OS_UpdateMEMC: bit 11 stops and starts the sound DMA); a program's own Level 1 handler, or the OS's channel handler
  running voice generators (install, attach by number or name, Instantiate/
  Free, GateOn/Fill/Update/GateOff as SoundChannels 1.27 sequences them,
  durations - 0 leaves a playing note's alone, so a note can be re-pitched
  each frame - Sound_Pitch, BBC-style amplitudes and pitches); WaveSynth-
  Beep (voice 1, every channel's to start with) made natively; the
  scheduler (Sound_QSchedule, QTempo); to SDL or a WAV file
- the command line: Set, SetEval (integer and string expressions), Unset,
  If ... Then ... Else, Dir, Load, Save, Copy (R: recursively), Delete, RMLoad,
  RMRun, RMKill, RMClear, FX, IconSprites, ChannelVoice, Filer_OpenDir and
  Filer_CloseDir (directory viewers, on the desktop), and loaded modules' commands, after GSTrans
  (<var>), abbreviated too ("L." for Load: the first command it abbreviates);
  running a file by name: a BBC font file (&FF7) printed, a module (&FFA)
  RMRun; OS_GSTrans; OS_ReadVarVal, OS_SetVarVal (strings, and numbers as 4-byte
  integers). Other commands are logged and ignored
- the Wimp, without a desktop (but see The desktop, below): Wimp_Initialise (version 3.16), Wimp_Poll
  (events queued by the game's code), templates (Wimp_OpenTemplate,
  LoadTemplate, CloseTemplate), windows and icons kept as the program
  defines them but not shown (create, open, close, state, information,
  outline, extent, icon state, WhichIcon, the icon bar; redraws find
  nothing to draw), menus accepted, dialogue boxes opened as menus,
  option icons selected and toggled by clicks as the Wimp does, the caret,
  messages to itself, a Filer for saving (game code names where the next
  save dragged out goes: the data transfer protocol's DataSave/Ack,
  DataLoad/Ack), Wimp_GetPointerInfo, Wimp_ReportError (to stderr),
  Wimp_SpriteOp on the Wimp's sprite pool (*IconSprites), Wimp_ReadSysInfo,
  Wimp_SetMode, Wimp_CloseDown; Wimp_SlotSize (the application slot is
  the RAM limit) and the screen's dynamic area (OS_ReadDynamicArea,
  OS_ChangeDynamicArea 2)
- the Shared C Library (src/clib.c): SharedCLibrary_LibInitAPCS_A/R, and
  the kernel's and the C library's functions on the host's C library -
  C89's behaviour as the library had it (long 32 bits, doubles in FPA
  order), its static data as clients' macros read it (errno, __iob,
  __ctype), stdio on the OS's files, the console through the VDU, the
  library's rand(); no floating point arithmetic in the client (no FP
  emulator), no stack extension (a 64K root stack instead)
- the kernel: OS_Byte (keyboard scans, single key or first key pressed, or
  from 16; the keyboard status (Caps Lock as the host's);
  INKEY, the OS version, events, Escape, banks, mode, the vsync counter,
  the text cursor), OS_Word (character definitions, palette, pointer - its shape, bounding
  box, and the mouse and pointer position, set and read - display
  start, the real-time clock: the host's, or fixed in headless runs),
  OS_Mouse, OS_GetEnv, OS_ReadMemMapInfo (4MB) and OS_ReadMemMapEntries
  (screen memory from physical 0, the sound page, the application slot), OS_ReadSysInfo 2 (the
  hardware: IOC/MEMC1/VIDC1, or a Risc PC's IOMD/VIDC20), OS_ServiceCall,
  OS_ChangeEnvironment (MemoryLimit and ApplicationSpace: the end of the
  slot, as the C library's heap reads them), OS_EnterOS, OS_ReadVduVariables,
  OS_ReadModeVariable, OS_ReadMonotonicTime, conversions (numbers, and
  dates: OS_ConvertDateAndTime, OS_ConvertStandardDateAndTime), OS_CLI (logged),
  modules' SWI chunks, OS_Module claims and look-ups by name (18),
  OS_CheckModeValid, the undefined instruction trap (OS_ChangeEnvironment
  1's handler: an ARM2's answer to an ARM3 cache-controller MRC; or a
  branch the program put in the vector at &4 itself),
  transient callbacks (OS_AddCallBack) as a SWI or an interrupt returns
  to USR mode, IOC timer 0 as the centisecond ticker (its count, latched by
  &320004C) and RISC OS 3's centisecond counter at &A38, modules'
  parameters (R10) at initialisation (roger_module.args),
  OS_SWINumberFromString (a few names programs probe for); key transition
  events (event 11, low-level key numbers, once a frame); Hourglass
  (nothing shown); OS_SpriteOp select, read sprite information and area
  operations in headless runs too (only plotting needs the VDU drivers);
  errors as error blocks (os_error)
- files: saves keep their type in the host name's suffix (OS_File 0/10,
  and 1-3/18 set it), the save directory looked up ignoring suffixes;
  "HostFS:" followed by a host path names that host file; a current
  directory (*Dir, OS_FSControl 0) where relative names are looked for
  first

The desktop
-----------

`--desktop` shows a Wimp program's desktop and lets the user work it, in
place of the game's wimp_idle hook: the icon bar along the bottom with
the program's icons on its right (or left, window -2), its menus,
windows and dialogue boxes, and what it draws in its windows itself - in
RISC OS 3.1's look, with libroger's backdrop. Once the program takes
the screen for itself (a mode change) its screen is shown - fullscreen,
unless the window already was or `--windowed-games` - and the desktop
again, in the window, when it next calls Wimp_Poll. src/desktop.c draws it
all, from the windows and icons wimp.c keeps:

- the screen: the window's size, pixel for pixel - resizing the window
  resizes the desktop, never scales it, and display shaders aren't used
  on it - each pixel 2x2 OS units (so 640x512 is the 1280x1024 OS units a
  program expects, and a larger window more desktop); `--desktop=WxH` sets
  the size to start with (1024x768), and a headless run's. Resized, the
  windows and menus keep their place from the top left, and windows are
  moved back onto the screen (the title bar first), as after a mode
  change. The backdrop
  (desktop/backdrop.png, embedded) is scaled to cover the screen above the
  icon bar; the Wimp's 16 colours and palette; the system font (the
  character set, below, drawn double height)
- the icon bar: the application's !Sprites (or !Sprites22) in the Wimp's
  sprite pool as the Filer would put them there, icons laid out from the
  ends, Mouse_Click on them
- windows: the stack (opened behind a window, at the top or the bottom),
  furniture as the flags ask (title bar, back, close, toggle size, scroll
  bars, adjust size) with the window's colours and input focus, icons (text,
  sprites from the window's area, the pool or the Wimp's own option,
  radio, directory, application and file sprites drawn here; borders,
  validation strings S, D, R and A; shaded and selected), user-drawn
  windows (Redraw_Window_Request, Wimp_RedrawWindow / UpdateWindow /
  GetRectangle: the program draws in screen memory with the VDU drivers,
  and that picture is kept for its window - a window off the program's
  screen, on a larger desktop, is moved onto it to be drawn),
  Wimp_ForceRedraw,
  Wimp_GetWindowOutline as drawn
- the user: clicks by button type (click, release, double click, drag,
  menu, radio, writable icons taking the caret), dragging and resizing
  windows, scroll arrows (auto-repeat), wells and sliders, Scroll_Request
  for windows that scroll themselves, back, close and toggle size as
  Open_Window_Request / Close_Window_Request (carried out by the Wimp if
  the program masks them), Wimp_DragBox (User_Drag_Box; a parent box
  that is the mode's screen, as a save box's usually is, taken as the
  desktop's), Wimp_GetPointerInfo
- menus: menu trees (titles, ticks, dotted lines, shaded and sprite items,
  submenus opened from the arrow - to the left when there's no room - or
  through Message_MenuWarning and Wimp_CreateSubMenu, dialogue boxes as
  submenus), kept on the screen; Select chooses and closes, Adjust keeps
  the menu open; clicking elsewhere or Escape closes it
- the keyboard: writable icons edited by the Wimp (A validation, Delete,
  Copy, cursor left and right, Ctrl-U), the rest as Key_Pressed in the
  Wimp's key codes, Lose_Caret / Gain_Caret
- the Filer (src/filer.c), a task of its own run in-process: directory
  viewers (`roger_filer_open()`, *Filer_OpenDir) of one directory each -
  the program's saved copies as well as the originals - in RISC OS 3's
  large icons on a fixed grid (each cell as wide as the longest name,
  ten characters at least; as many columns as fit, laid out again when
  the window is resized), with "file_xxx", "directory" or an
  application's own sprite (its !Sprites22 or !Sprites merged into the
  pool), read again once a second. Select selects, Adjust toggles;
  double-click opens a directory (Adjust: instead of this one), runs an
  application, or offers a file to the program (Message_DataOpen - if
  it doesn't take it, it is run); dragging an icon to the program's
  window or icon bar icon gives it the file (Message_DataLoad); and a
  save box's icon dragged to a viewer saves there (Message_DataSave,
  DataSaveAck with the name in that directory, DataLoad, DataLoadAck).
  The first directory the game opens has an icon on the icon bar's left,
  as a disc's: Select or Adjust opens its viewer again (where a new one
  opens), and a save dropped on it saves there.
  Running is the game's to say (roger_config.filer_run: there's only the
  one program; `roger_filer_load()` gives the program a file or
  application as if dragged to it)

Input is taken while the program is in Wimp_Poll, from the same mouse and
keyboard state as everything else, so `--script` drives the desktop in
headless runs too, and their screenshots show it. Without `--desktop`
nothing changes: the program's Wimp calls behave as before, and headless
runs give the same output.

The character set
-----------------

The 8x8 character set - the VDU drivers', and the desktop's system font
drawn double height - is "Acorn Mode 0" by p1.mark
(https://fontstruct.com/fontstructions/show/1271339), licensed under
Creative Commons Attribution-ShareAlike 3.0
(http://creativecommons.org/licenses/by-sa/3.0/). font/acorn-mode-0/ holds
the font as distributed (with its license.txt and readme.txt); src/font.c
is a bitmap table made from it by font/mkfont.py, and is an adaptation
under the same licence. The Latin-1 characters the font hasn't come from
the X11 misc-fixed 5x8 font (public domain).

Display shaders
---------------

Keys in the window: F10 / Shift+F10 cycle through the display shaders, F11
fullscreen, F12 screenshot (Ctrl+F10/F11/F12 for a game that sets
roger_config.ctrl_meta_keys because it uses those keys itself) (with a shader on, the shaded frame is saved too,
as screenshot-N-NAME.bmp). The shaders are RetroArch slang presets run on
SDL_GPU (src/shader.c), with SDL 3.4's GPU renderer and a device that takes
SPIR-V (Vulkan); without one there's only the plain view.

    none                each game pixel a sharp rectangle (the default)
    bilinear, lanczos3, spline36             resampling
    xbr-lv2, xbrz, scalefx                   pixel-art upscalers
    crt                 libroger's own (before: --crt)
    crt-lottes, crt-hyllian, crt-geom, crt-easymode,
    crt-guest-advanced, crt-royale           CRT simulations

Most come from libretro's slang-shaders, under their authors' licences:
MIT, GPL (2.0 or later; xBRZ's code GPL-3.0) or public domain - see
shaders/README. With them included, a program linked with libroger is
distributed under the GPL, version 3.

Not implemented (yet): outline fonts (Font_Paint draws pixel fonts only,
and on the desktop only in the program's own screen), other tasks (but the Filer's viewers - which have no menu,
small icons or full info, selection by dragging a box, or copying between
them), pointer
entering and leaving events, keyboard shortcuts for windows (hot keys);
VDU colours and
sprites at 16 and 32 bits per pixel, a 256-entry palette (8bpp modes keep
VIDC1's), most OS_CLI commands, FIQs.

Files
-----

    include/roger.h      the public interface: configuration, hooks, services
    include/roger_cpu.h  the interface to translated code
    src/cpu.c            CPU state, modes, images and dispatch, gaps, interpreter
    src/irq.c            time, interrupts, vectors, device vectors
    src/video.c          modes, screen memory, MEMC, VIDC, the displayed frame
    src/vdu.c            VDU drivers (graphics, text) and sprite plotting
    src/font.c           the character set (generated by font/mkfont.py; CC BY-SA
                         3.0, see The character set)
    src/sound.c          the sound system
    src/audio.c          host audio (SDL stream, WAV)
    src/files.c          files and sprite files
    src/fonts.c          the Font Manager (pixel fonts)
    src/disc.c           the floppy, sector by sector (DiscOp), FileCore and ADFS stand-ins
    src/modules.c        relocatable modules, the RMA
    src/cli.c            OS_CLI (* commands) and system variables
    src/wimp.c           the Wimp, the application slot, the screen's dynamic area
    src/desktop.c        the desktop (--desktop): drawing it, and the user's input
    src/filer.c          the Filer: directory viewers, the data transfer protocol's other end
    src/backdrop.S       embeds desktop/backdrop.png
    src/swi.c            SWI dispatch and the kernel's SWIs
    src/host.c           the SDL3 window, keyboard and mouse
    src/main.c           start-up, command line, event log, scripts, finish
    src/clib.c           the Shared C Library
    src/shader.c         display shader chains on SDL_GPU
    shaders/             the display shaders: build.py, own/, slang/ (from
                         libretro's slang-shaders), generated/chains.c; see
                         shaders/README

Game controllers
----------------

Controllers SDL knows as gamepads (Xbox, PlayStation, ...) play as the
keyboard: each control holds a RISC OS key while it's pressed, so every
way a program reads keys (INKEY, OS_Byte 121, key transition events, the
keyboard buffer) sees it, and no program needs to know. The controls are
up down left right (the D-pad, and the left stick past half way), a b x y,
lb rb lt rt, back start guide, ls rs. A game's `pad_map` ("left=Z,right=X,
a=Return,...", keys by SDL's names, or inkeyN) goes over libroger's default
(the cursor keys, a Space, b and start Return, back Escape), and --pad=
over that; a second controller has `pad2_map` (default: the first's). In
headless runs, --script's pad:control[:ms] and pad2:... press one (src/pad.c).

Joysticks: a program that reads a joystick interface says so
(`roger_config.joystick`), and when a controller is connected at start (or
with --joystick; --joystick=off never) libroger answers the interface's
SWIs from the controllers, ahead of any module that would read its
hardware: ROGER_JOY_ACORN, Acorn's Joystick module (Joystick_Read: y and
x signed, -64/0/64 as the A3010's digital ports give, or with
ROGER_JOY_ANALOGUE the stick's position; bit 16 a, 17 b), and
ROGER_JOY_KM, Keith Martin's interface in The Fourth Dimension's games
(Joy_Read0/1: five lines, high when pressed - right, left, down, up,
fire; and the port itself, the byte at &33A0004, for a program that reads
it: Enter the Realm). The D-pad, left stick, a and b are then the joystick's, not keys;
the other controls keep their keys. Joystick 0 is the first controller,
1 the second.
