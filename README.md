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
  (`loader_programs`, interpreted)
- its files: the host directory standing in for the application directory
  and the RISC OS prefixes that mean it, and system variables as !Run sets
  them (`variables`; `<Game$Dir>` in a filename or command expands)
- the machine as !Run left it: screen mode, WimpSlot, screen memory size and
  placement, initial stack
- interrupts: a raw IRQ handler at &100 (`irq1v`) for games that bypass the
  kernel's device vectors
- input: pointer offset, relative mouse capture, the headless scenario
- translation gaps that are known and may be interpreted
- hooks: init, every frame, every centisecond, first look at SWIs and at
  dispatch, extra command-line arguments, the program returning to the OS
  (`exited`: what !Run does next), finish

Game code (hooks, or a loader written in C - e.g. a BASIC program
translated by hand) can call SWIs and * commands with `roger_swi()` and
`roger_cli()`, and read and set system variables.

Command line (any game): `--limit=N` (headless, deterministic),
`--script=...` (scripted input: keys, clicks, taps, screenshots),
`--trace-gaps`, `--full-os`, `--no-title`, `--scale=N`,
`--fullscreen[=WxH@Hz]`, `--crt` (CRT shader), `--no-sound`, `--wav=F`,
`--app=DIR`, `--save=DIR`, `--out=PREFIX`; `--help` lists them.

Determinism: headless runs (`--limit`) advance time by instruction count
(a centisecond per 20000, a frame per 40000) and write the event log,
VDU stream, screen memory and registers, so two builds of a game can be
compared byte for byte.

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

- CPU: processor modes with banked registers, TEQP mode changes, the
  ARM2 interpreter (gaps, self-modified instructions), SMC detection
- interrupts: TickerV, EventV (vsync, mouse; a claimant can intercept with
  the return address on the stack), OS_ClaimDeviceVector (vsync,
  IOC timer 1), the raw handler at &100; palette changes part way down the
  frame (timer 1 started at flyback, VIDC VDSR/VDER/VCR timing)
- video: the standard 50Hz graphics modes (0, 1, 2, 4, 5, 8, 9, 10, 12, 13,
  15), screen banks (OS_Byte 112/113, OS_Word 22), MEMC Vinit/Vstart/Vend
  with wrap-around, VIDC palette including 256-colour modes, VDU and mode
  variables, VDU drivers (MODE, CLS/CLG, COLOUR/GCOL, tints, windows,
  origin, PLOT lines/points/triangles/rectangles; VDU 21/6; no text),
  OS_SpriteOp on user areas (load, read area information, select, plot)
- files: OS_File (load, save, catalogue), OS_Find, OS_GBPB, OS_Args,
  OS_BGet/BPut on the host directory, catalogue information from `,xxx` /
  `,load-exec` name suffixes, writes redirected to a save directory (and
  read back from there)
- sound: DMA at the configured rate with VIDC log samples and stereo
  positions; a program's own Level 1 handler, or the OS's channel handler
  running voice generators (install, attach by number or name, Instantiate/
  Free, GateOn/Fill/GateOff, durations, Sound_Pitch, BBC-style amplitudes
  and pitches); to SDL or a WAV file
- the command line: Set, SetEval, Unset, If ... Then ... Else, Load, Save,
  RMLoad, RMRun, RMKill, FX, after GSTrans (<var>); OS_ReadVarVal,
  OS_SetVarVal. Other commands are logged and ignored
- the kernel: OS_Byte (keyboard scans, INKEY, events, Escape, banks, mode),
  OS_Word (palette, pointer, display start), OS_Mouse, OS_GetEnv,
  OS_ChangeEnvironment, OS_EnterOS, OS_ReadVduVariables,
  OS_ReadModeVariable, OS_ReadMonotonicTime, conversions, OS_CLI (logged),
  modules' SWI chunks, OS_Module claims

Not implemented (yet): text rendering, the Wimp, most OS_CLI commands,
keyboard events, sound tuning (Sound_Tuning, *Tuning), FIQs.

Files
-----

    include/roger.h      the public interface: configuration, hooks, services
    include/roger_cpu.h  the interface to translated code
    src/cpu.c            CPU state, modes, images and dispatch, gaps, interpreter
    src/irq.c            time, interrupts, vectors, device vectors
    src/video.c          modes, screen memory, MEMC, VIDC, the displayed frame
    src/vdu.c            VDU drivers and sprite plotting
    src/sound.c          the sound system
    src/audio.c          host audio (SDL stream, WAV)
    src/files.c          files and sprite files
    src/modules.c        relocatable modules, the RMA
    src/cli.c            OS_CLI (* commands) and system variables
    src/swi.c            SWI dispatch and the kernel's SWIs
    src/host.c           the SDL3 window, keyboard and mouse
    src/main.c           start-up, command line, event log, scripts, finish
    shaders/             the --crt fragment shader (shaders/build.sh rebuilds
                         the embedded SPIR-V)
