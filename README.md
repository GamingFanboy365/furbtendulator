# furb: Furbtendulator for Linux

Furbtendulator (https://github.com/FurblandChannel/Furbtendulator) is a fork of
NintendulatorNRS, the reference emulator for VT02/VT03/VT09/VT32/VT369
"OneBus" famiclones, UM6578 and the other obscure NES-compatible hardware and
bootleg mappers.  It is a Windows program.  This package builds it for Linux
as two programs from one set of emulator objects, and for Android as an app:

| Program | What it is |
|---------|------------|
| `furb` | Furbtendulator as you know it: its window, menus, dialogs, debugger, sound, keyboard and gamepads (GTK 3 and SDL 2) |
| `furb_cli` | headless: runs a ROM for a set number of frames with scripted input and writes frames, sound, video, savestates, movies and traces, for comparing against other emulators and for debugging without screenshots (made for PocketVT and AI-assisted debugging) |
| Furbtendulator for Android | the same emulator on phones, tablets, TVs and Chromebooks: on-screen touch controls, gamepads and keyboards, save states (`android/`; see [Android](#furbtendulator-for-android)) |

The emulation code is Furbtendulator's own, unmodified; all three run the
same machine and `furb_cli`'s output is byte-for-byte reproducible.  What
Windows supplies -- windows, dialogs, DirectDraw, DirectSound, DirectInput,
the registry -- is supplied by a compatibility layer (`compat/`, `gui/`).  It
builds natively on 64-bit Linux, as 32-bit x86 and for ARM, with GCC or
clang, and the builds produce identical output (tested on x86_64, i386, arm64
and 32-bit ARM; see [Determinism](#determinism-and-3264-bit-and-arm-builds)).

## Running the included binaries

`bin/` holds x86_64 builds made on Ubuntu 24.04: they need glibc 2.38 or newer
(Ubuntu 24.04, Debian 13, Fedora 39 or equivalent; WSL's default Ubuntu is
fine).  `furb` also needs GTK 3 and SDL 2, which desktop systems have:

```
sudo apt install libgtk-3-0 libsdl2-2.0-0     # Debian / Ubuntu, if missing
bin/furb game.nes                             # the GUI
bin/furb_cli game.nes --frames 900 --dump 300,899 --out shots/game
```

On an older system use `bin32/`: 32-bit x86 builds of both programs, `furb`
and `furb_cli`, that need only glibc 2.29 (Ubuntu 19.04, Debian 11 or
equivalent).  They run on 64-bit systems through the 32-bit libraries:

```
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install libc6:i386                                # furb_cli
sudo apt install libgtk-3-0:i386 libsdl2-2.0-0:i386        # furb as well
bin32/furb game.nes
```

They are built from the same sources against Ubuntu 20.04's compiler, C
library, GTK 3 and SDL 2 (the newest glibc symbol they use is 2.27; `furb`
wants GTK 3.22 or newer at run time).  `bin32/furb_cli` gives the same output
as `bin/furb_cli` on all 311 comparison ROMs, and its self-test passes on
glibc 2.29 itself; `bin32/furb` passes `tools/gui_smoke.py` on Ubuntu 19.04's
own libraries (glibc 2.29, GTK 3.24.8, SDL 2.0.9) and on Ubuntu 20.04's.  On
another architecture, build from source (below; about two minutes).  Keep `bin/` (or `bin32/`) together: `bin/Mappers/*.so` are the iNES, FDS, NSF
and VS mapper packs, loaded at startup the way the Windows build loads
`Mappers\*.dll`, and the `.cfg` files, `BIOS/` and `samples/` sit next to the
programs as they do next to `Furbtendulator.exe`.

## furb: the GUI

```
furb [ROM] [--data-dir DIR] [--config FILE] [--set Name=Value]...
```

Everything is Furbtendulator's own: the menu bar, the accelerator keys (F2
run, F3 stop, F4/Shift+F4 reset, F5/F8 save/load state, F6/F7 state slot,
Ctrl+F frame step, Tab slowdown, Alt+Enter fullscreen, F10 header editor,
Ctrl+F1/Ctrl+F2 the CPU and PPU debuggers, Ctrl+D DIP switches,
Shift+Ctrl+C cheats, ...), and every dialog -- input setup with key capture,
the palette editor, the debuggers with their live pattern/nametable/sprite
views, the header editor, cheats, DIP switches, the NSF player, movies and
the rest.  They run Furbtendulator's dialog procedures against GTK widgets
built from its `.rc` dialog templates (the mapper packs' windows from the
packs' own `.rc` scripts).
ROMs can also be dropped on the window.

Settings are kept in `~/.config/furbtendulator/settings.reg` (the
`HKCU\SOFTWARE\Nintendulator` key as a regedit export, so a Windows export
can be copied in) and written on exit; `--config` uses another file and
`--set` overrides a value for this run.  Savestates, battery saves, FDS disk
changes and dumps go to `~/.local/share/furbtendulator/Nintendulator` (or
`DIR/Nintendulator` with `--data-dir`), the layout Furbtendulator uses under
`%APPDATA%\Nintendulator`.

On the first run, controller 1 is mapped to the keyboard: arrows, X = A,
Z = B, S and A = turbo A and B, Enter = Start, Right Shift = Select.  Input >
Setup changes it; joysticks and gamepads are listed there too.  Sound plays
through SDL, and the emulation is paced by it exactly as by DirectSound on
Windows.

Not available: AVI capture (it uses Video for Windows; `furb_cli --avi` records
video), CopyNES plugins, and the karaoke microphone's level (the Famicom
microphone, Ctrl+M, works).

## furb_cli: the headless program

```
furb_cli ROM --frames 900 --dump 300,899 --out shots/scr \
         --input "320-325:Start;500-900:Right" [--port 2] [--hashes] [--set VT03Palette=1]
```

ROM can be anything Furbtendulator opens: `.nes` (iNES/NES 2.0), `.unf`,
`.fds`/`.qd`, `.nsf`/`.nsfe`, Vs. System dumps. Run `furb_cli` with no
arguments for the option list.
### Frames

For each dumped frame it writes `PREFIX_fNNNN.ppm` (exactly the region
Furbtendulator's own screenshot saves), `.idx` (the PPU's raw uint16 palette
indices), `.txt` (`$2000-$20FF`, `$4100-$41FF` and palette RAM; the full
1024-entry palette on VT369) and `.ram` (CPU RAM). On VT369 in hi-res mode
(`$201C` bit 2) the `.idx` interleaves the even/odd half-pixel arrays, as the
GUI does. `--every K` dumps every Kth frame, `--hashes` prints one hash per
frame for frame-set tests, `--info` prints the ROM, its DIP switch and cheat
definitions and exits. Frames count from power-on starting at 0. The run is
deterministic: the same arguments give the same frames.
### Input script

`--input` takes `FRAMES:ACTION` items separated by `;` (the option can be
repeated). FRAMES is `N` or `FIRST-LAST`. Actions:

| Action | Meaning |
|--------|---------|
| `A+B+Select+Start+Up+Down+Left+Right+TurboA+TurboB`, `bN` | pad buttons (default pad: `--port`) |
| `p1:` .. `p4:`, `exp:` prefix | send them to that pad (Four Score / 4-player adapters: p1..p4) or the expansion device |
| `trigger` | Zapper and other light guns (button 0) |
| `key:NAME+NAME` | keyboard keys by DirectInput name (`a`, `return`, `space`, `f1`, `lshift`, ...) for Family BASIC, Subor and the other keyboards |
| `mouse:X,Y[+left][+right][+middle]` | cursor in NES pixels, for the Zapper aim, mice, Arkanoid paddle and tablet |
| `mic:LEVEL` | karaoke microphone peak level 0..1 |
| `cmd:reset`, `cmd:hardreset` | soft / hard reset |
| `cmd:coin1`, `cmd:coin2` | Vs. System coin slots |
| `cmd:button` | the plug-through device's button (the GUI's "press button" menu item) |
| `cmd:mic` | Famicom controller-2 microphone |
| `cmd:fds-insert`, `fds-eject`, `fds-next`, `fds-prev` | disk side changes |
| `cmd:save=FILE`, `cmd:load=FILE` | savestate to / from a file |
| `cmd:dip=VALUE` | set the DIP switches (hard-resets like the GUI) |
| `cmd:nsf-song=N` | select song N in the NSF player and press Play |
| `cmd:tape-play=FILE`, `tape-record=FILE`, `tape-stop` | Family BASIC / Subor data recorder |
### Devices

`--device PORT=TYPE` plugs a controller into `port1`, `port2`, `fs1`..`fs4`
or `exp`; `--list-devices` prints the type names (standard, zapper,
arkanoid, powerpad-a/b, fourscore, snes, vs-zapper, snes-mouse, subor-mouse,
and on the expansion port fami4play, hori-4play, family-basic-keyboard,
subor-keyboard, family-trainer-a/b, tablet, hyper shots, turbo-file,
sharp-c1-cassette and more). `port1=fourscore` or `exp=fami4play`/`hori-4play`
connects the extra pads so `p3:`/`p4:` work.
### Settings

Settings go through Furbtendulator's own registry loader, so every GUI
setting is available. `--config FILE` loads a regedit export of
`HKCU\SOFTWARE\Nintendulator` (UTF-16 or UTF-8, as regedit writes it) or a
plain `Name=value` file; `--set Name=Value` sets one value (numbers become
DWORDs, `"text"` becomes a string); `--save-config FILE` writes the settings
in effect at exit as a `.reg` file. `--palette FILE.pal` loads a custom
palette for every region.

`--data-dir DIR` (default `~/.furb_cli`) is the Windows GUI's data folder: battery
saves go to `DIR/SRAM` on exit, as they do in the GUI. `--no-save` writes none.
### Audio, video, states, movies

`--wav FILE` records the sound Furbtendulator's mixer produces (48 kHz,
mono, 16-bit, the samples DirectSound would have played). `--avi FILE`
writes uncompressed video plus that audio at the console's exact frame rate
(NTSC 60.0988, PAL/Dendy 50.007). `--load-state FILE` starts from a
savestate; `cmd:save=` and `cmd:load=` save and load mid-run.
`--movie-record FILE.nmv` and `--movie-play FILE.nmv` drive the movie
dialogs, so the files are the GUI's own format.
### Game options, cheats, debugging

`--dip VALUE` sets the DIP switches (see `--info` for the definitions from
`dip.cfg`). `--cheat NAME` enables every cheat from `cheats.cfg` whose name
contains NAME (`all` for all). `--header BYTE=VALUE`, `mapper=N` or
`submapper=N` patch a copy of the iNES header before loading (the original
is untouched). `--trace FILE` writes the debugger's CPU trace log, for
`--trace-frames A-B` only if given.
### NSF

Furbtendulator's NSF player loads a tune and then waits for Play, in the GUI
too. `--nsf-song N` presses Play on song N at frame 0; `cmd:nsf-song=N`
switches song later.
### Data files

`bin/` holds `*.cfg` (`cheats.cfg`, `dip.cfg`, `fastload.cfg`),
`BIOS/` and `samples/` from the Furbtendulator release, next to the binary
where the GUI keeps them (`build.py` copies them from `Furbtendulator-src/bin-data`). Firmware is not included. For FDS put
`DISKSYS.ROM` in `BIOS/`; VT369 carts that need their internal ROM want
`BIOS/VT369-00.BIN` (file names are matched case-insensitively).
### Self-test

```
python3 selftest.py [--rom testroms/Scramble.nes]
```

Builds tiny ROMs, an NSF, a battery ROM and an FDS image on the fly and
checks every feature above: pads, Four Score, both 4-player adapters,
Zapper, microphones, Vs. coins, savestates, config round trip, trace, header
patch, WAV/AVI, NSF songs, battery saves, palette, cheats, DIP switches, FDS
commands. With `--rom` it also checks that a recorded movie and a savestate
continuation replay frame-identically on a real game.

## Furbtendulator for Android

`android/` is Furbtendulator as an Android app, for Android 5.0 or newer on
64-bit and 32-bit ARM and x86 (phones, tablets, Android TV, Chromebooks).  It
runs the same emulator objects as `furb_cli`, frame by frame on one thread,
so its emulation is `furb_cli`'s: what a game does on the phone it does in
`furb_cli` on a PC, frame for frame.  The picture, sound and controls are a
front end on SDL 2 (`android/native/main.cpp`); the menus, the file picker
and the settings are a small Java activity (`android/app`).  The debugger
windows and the other desktop dialogs are not part of it: for those, and for
AI-assisted debugging, use `furb` and `furb_cli` on a PC.

`bin-android/Furbtendulator.apk` is a ready-made build for all four ABIs.
Install it by opening it on the device (Android asks to allow installs from
that app, the browser or file manager, the first time) or with
`adb install bin-android/Furbtendulator.apk`.  It has run on an emulated
phone so far, not yet on a real one (see *What was tested*).

### Using it

The first screen asks for a game: MENU (or the Back button, or a gamepad's
Guide button, or Select+Start together) opens the menu, and *Open game...*
picks a file through Android's file picker.  `.nes`, `.unf`, `.fds`, `.nsf`
and the other formats Furbtendulator opens work, and so do compressed files:
ZIP, 7z and gzip.  An archive holding one game opens it; one holding several
lists them to choose from, with a search box when there are many (any words
typed, anywhere in the name).  RAR archives are not supported.  A picked game
is copied into the app's own storage, so it is listed under *Recent games*
afterwards and its battery saves and save states stay with it.  File
managers can also open a game or an archive in the app directly (*Open
with*).

An NSF file (NES music) opens in a player: its title, artist and copyright,
the expansion sound it uses (VRC6, VRC7, FDS, MMC5, Namco 163, Sunsoft 5B or
OneBus, all played by Furbtendulator's NSF support), the song number and the
track's name where the file has one (NSFe and NSF2), the time played and the
sound's waveform, with PREV, PLAY/STOP and NEXT buttons.  The starting song
plays as soon as the file opens.  On a gamepad or keyboard, left and right
change the song (up and down by ten), A or Start plays or stops, B stops.
The music stops when the app leaves the screen.

The menu has save and load state (ten slots, shown with the time they were
saved), reset and power cycle, the disk side for Famicom Disk System games,
*Insert coin* for Vs. System games, a song list for NSF files, the region (NTSC,
PAL, Dendy), the settings, *Install a BIOS file...* and *Exit*.  The emulator
pauses while the menu is open.  *Install a BIOS file...* copies a firmware
file into the app's `BIOS` folder under the name Furbtendulator looks for,
recognised by its CRC32 from `BIOS/dir.txt` whatever the file is called:
`DISKSYS.ROM` for the Disk System, `VT369-00.BIN` for the VT369 carts that
need their internal ROM, and so on.

On the screen, a d-pad (eight directions; slide between them), A and B
(touch between them for both), Select, Start, MENU and `>>`, which runs the
game at four times the speed while held.  In portrait the picture sits on top
with the controls below it; in landscape the controls lie over the sides of
the picture.  Light-gun games (Zapper) aim and fire where the picture is
touched.

Gamepads (USB or Bluetooth) work as player 1, 2, ... in the order they were
connected: the d-pad or left stick, the right face button is A and the bottom
one is B (as on the NES pad), the other two are turbo A and B, Back/Select and
Start are Select and Start, and the right shoulder button fast-forwards.  A
physical keyboard is player 1 with the desktop GUI's keys (arrows, X = A,
Z = B, S and A = turbo, Enter = Start, Right Shift = Select; Esc or F1 for
the menu, F5/F8 to save/load the current state slot, Tab to fast-forward),
and it is also the keyboard of the Family BASIC, Subor and other famiclone
keyboard devices.  The on-screen controls hide while a gamepad or keyboard
is in use and come back at the next touch.

The settings are square or TV-shaped (8:7) pixels, a smoothed or sharp
picture, the on-screen controls on or off and their opacity, vibration on
button presses, sound, swapped duty cycles (Furbtendulator's Sound menu
option: the 25% and 50% square waves exchanged, as on some clone consoles'
sound chips), and a counter of frames per second and the screen's refresh
rate.

The emulation is paced by the screen (`android/native/pacing.h`): on a 60 Hz
screen every refresh shows the next frame, on a 120 Hz one every other
refresh, and on a phone running at 90 Hz the app asks for its 60 Hz (or
120 Hz) mode where it has one.  The game then runs at the screen's speed
(0.16% slower than an NTSC NES on a 60.00 Hz screen) and the sound is
stretched by a fraction of a percent to match.  Pacing by the sound, as the
first versions did, skipped and doubled frames many times a second, because
phones take sound in blocks of 1024 samples or more: `tools/pacing_sim.cpp`
simulates both against different screens and sound devices.
Furbtendulator is an accurate, and so
demanding, emulator: on this port's test machine one x86_64 core ran VT03
and VT369 test ROMs at about 230-240 frames per second and plain NES ones at
about 350, so recent phones have room to spare, but a slow budget phone may
not reach full speed.

### Building the APK

```
tools/build_android.sh [--accept-sdk-license] [WORKDIR]     # -> bin-android/Furbtendulator.apk
```

It needs the Android SDK with an NDK (r27 or newer; `ANDROID_HOME`, and
`ANDROID_NDK_HOME` or an NDK in the SDK's `ndk/` folder), git, cmake, ninja,
python3, a JDK 17 or newer, and network access to dl.google.com,
maven.google.com and github.com.  Without an SDK, `--accept-sdk-license`
downloads Google's command-line tools into WORKDIR (default `build-android/`)
and installs the platform, build tools and NDK there; that means accepting
the Android SDK license, so pass it only if you do.  The script fetches
SDL 2.32.10 with git, builds it and `build.py --android ABI` (the emulator,
the front end and the mapper packs with the NDK's clang) for each ABI, and
has Gradle package them with the activity, SDL's Java half and the data files
into a signed APK.  `FURB_ABIS=arm64-v8a` limits it to one ABI for a quicker
test build.  `android/` is also an ordinary Gradle project that Android Studio
opens, once the script has filled `android/native-out/`.

The APK is signed with `FURB_KEYSTORE` (with `FURB_KEYSTORE_PASSWORD`,
`FURB_KEY_ALIAS`, `FURB_KEY_PASSWORD`) or, without one, with
`android/furb-release.keystore`, which the script makes the first time
(password `furbtendulator`; it is not committed).  Keep that file: Android
installs an update over an earlier version only when both are signed with the
same key, and uninstalling instead deletes the app's saves.  The included
APK is signed with such a key of its own: an APK built with another key
installs only after it is uninstalled.

### Inside

The mapper packs are the app's libraries `libfurb_iNES.so`, `libfurb_FDS.so`,
`libfurb_NSF.so` and `libfurb_VS.so`: the shim's `LoadLibrary` falls back to
loading `Mappers\iNES.dll` by that library name on Android, and hands each
pack the host's function table right after loading it (`furb_pack_attach`),
since Android does not let a library see the app's symbols the way Linux lets
a `.so` see the executable's.  `libmain.so` is the emulator, the headless
host of `furb_cli` and the front end; SDL's Java side starts its `SDL_main`.
The data files (`*.cfg`, `BIOS/`, `samples/`) are unpacked into the app's
files folder, which is Furbtendulator's program folder; the settings are kept
there in `settings.reg` (the front end's own settings are `Touch*` values in
the same file), and savestates, battery saves and disk changes under
`data/`.  Battery saves and settings are written when the app goes to the
background, since Android may end it there without warning.  The activity
talks to the native side in text commands (`open PATH`, `save N`, `reset`,
`pref sound 0`, ...), and the native side asks for the menu with a
`key=value;...` description of what is loaded.

### furb_touch: the front end on a PC

`python3 build.py --touch` also builds `furb_touch`, the Android front end
on desktop SDL 2, to test it without a phone: the mouse is a finger, and the
commands the activity would send are read from stdin (`furb_touch --help`
lists them; `menu` and `state` print what the menu would show).
`tools/touch_smoke.py --furb build/furb_touch` starts it on Xvfb in a
landscape and a portrait window and checks that a game opens from a command
and draws, that the on-screen A button and the keyboard reach the pad, that
MENU asks for the menu, that save states, reset and the settings work, that
it exits cleanly with its settings saved, and that the NSF player plays a
generated NSF, changes songs with NEXT and stops.

### What was tested

The NSF player and the archives were tested the same way: three NSF files
(Frogger, Missile Command, and Sudoku 6 in 1's 46 songs) play, change songs
and stop in portrait and landscape, and a ZIP and a 7z of nine games and NSFs
list them, find them by search and open the one picked, while a one-game ZIP
and a gzip file open directly.  The OneBus and other expansion-sound NSFs
were not tried in the app.

The APK ran on Android 7.0 in Google's Android emulator (an x86_64 system
image, without hardware acceleration): it starts, finds its mapper packs,
opens games through the file picker and through *Open with*, plays them
(Star Ally and Lucky Lawn Mower on VT03, VT09 and VT369, Zelda II, Pin Ball,
Lonely Island; the on-screen START button reaches the game), saves and
loads states, turns to landscape, and exits and starts again cleanly; an
update installed over it keeps the games and saves.  `furb_touch` passes
`tools/touch_smoke.py` built with GCC and with clang.  The emulation was
compared across the CPUs Android runs on, built with clang 18 as the NDK r27
builds it: arm64 and 32-bit ARMv7 (under qemu) and 32-bit x86 give exactly
the output of the x86_64 build on all 311 comparison ROMs and these games
(below).  Untested: a real phone or tablet (real touch screens, speed,
sound latency), the ARM builds inside Android (the emulator ran the x86_64
one), gamepads and keyboards on Android, Android TV, Chromebooks, and newer
Android versions.

## Determinism and 32/64-bit and ARM builds

The Windows sources assume `long` is 32 bits; `prep_src.py` rewrites the
build copy's `long` to `int` (identical on i386), so the native 64-bit build
behaves exactly like the 32-bit one.  The palette generator, the sound filters
and the expansion-sound tables call `sin`, `pow`, `exp` and friends, whose last
bit differs between libm builds (i386 glibc uses x87 code); they are routed to
a bundled copy of musl's portable implementations (`compat/libm`, MIT
licensed, built without FMA contraction), so i386, x86_64 and ARM compute
the same colours and samples.  Everything is also built without fused
multiply-add (`-ffp-contract=off`: ARM compilers fuse `a*b+c` by default, and
the palette generator's hue search then picks other colours for a few
palette entries) and with a signed `char` (ARM's is unsigned), and the sound
filters' switch to flush denormals to zero, done with SSE intrinsics on
Windows, sets the same mode on ARM (`compat/furb_math.h`).  The PPUs read and
write their tile and sprite buffers a word at a time at any byte offset,
which 32-bit ARM faults on once the compiler pairs two such accesses into
one 64-bit load; `prep_src.py` gives those accesses an unaligned type
(`furb_ua32`).  Two upstream reads of uninitialised memory
(the first audio sample; CPU/PPU members the constructors never set) are
closed: the sound buffer and every `new` allocation start zeroed.

`tools/compare_builds.py A/furb_cli B/furb_cli ROMS...` runs two builds over
a set of ROMs and reports any difference in video hashes, WAV, RAM or
registers.  `tools/fuzzrom.py DIR` generates OneBus test ROMs for VT02, VT03,
VT09, VT32, VT369 and UM6578 that hammer the VT PPU/CPU registers, palettes,
4bpp modes and sprites.  These builds agree on all 311 ROMs of the public NES
test-ROM collection (christopherpow/nes-test-roms) plus these: GCC on i386
and x86_64 (natively) and on arm64 (under qemu), and clang 18 on x86_64,
i386, arm64 and 32-bit ARMv7 (the last two under qemu).

## What is not tested

Real FDS disks (only a dummy BIOS was available), the keyboards, mice,
Arkanoid, tablet and data recorder (the plumbing works, no test ROM uses
them), and the VT369 hi-res dump path (Lucky Lawn Mower VT369 runs but never
enables hi-res). Anything tied to a real window (fullscreen, window size,
the palette editor's and debugger's views) has no meaning in `furb_cli`: the
settings those dialogs save can still be set with `--set` or `--config`, and
`furb` has them all.  The GUI's
sound was tested with SDL's dummy driver (no sound card here), its joystick
support without a joystick, and its windows under Xvfb with openbox (the
dialogs tried: input setup, controller config with key capture, palette,
header editor, CPU and PPU debuggers, cheats, DIP switches, NSF player, movie
recording, status window, file picker, fullscreen; the tape dialogs and the
mapper packs' game-specific configuration windows were not);
`tools/gui_smoke.py` repeats the basic checks (a ROM runs at 60 fps, the
keyboard reaches the pad, dialogs open and close, a clean exit saves the
settings).

## How the port works

`compat/` is a Win32/DirectX shim.  `compat/winapi.def` lists the Win32
functions Furbtendulator calls that need a host (windows, messages, menus,
dialogs, GDI, cursor, threads); `windows.h` declares them from it, the mapper
packs get forwarders to the executable generated from it, and the executable
exports them to the packs by name (`furb_host_lookup`, its only exported
symbol).  Each program brings its own host:

`furb_cli` (`furb_cli.cpp`, `compat/host_cli.cpp`) replaces `WinMain` with a
loop running the same CPU loop as `NES::Thread`.  Its window calls are inert;
dialogs are headless (a dialog script registered for a template ID runs the
real dialog procedure against fake controls, unscripted dialogs are
cancelled); file pickers are answered from a queue; DirectSound captures the
mixer's samples; pad buttons are mapped to virtual joysticks (DirectInput
device 2+port), so keyboard devices, which read real key codes, never collide
with them.

`furb` (`gui/`) runs Furbtendulator's own `WinMain` and emulation thread on a
real implementation: windows and a message queue on the GTK main loop, menus
and accelerator tables and dialog templates compiled from each module's `.rc`
script by `gui/rc2cpp.py` (a module's table is its `HINSTANCE`, so the packs'
dialogs come from their own `.rc`), the dialog controls as GTK widgets, GDI on
cairo, DirectDraw as memory surfaces shown in the window, DirectSound on SDL
audio, DirectInput from the windows' key and mouse events and SDL joysticks.
As on Windows, a window call from the emulation thread runs on the main thread
while the caller waits.

The Android app (`android/`, above) is a third host: `furb_cli`'s headless
one under a touch screen front end on SDL 2.

All share the registry (`compat/host_reg.cpp`: a map loaded from and saved
to `.reg` files).  `prep_src.py` copies the sources to the build folder, fixes
`#include` case and backslashes, rewrites `long` (above) and patches the
constructs that only MSVC accepts (some only with g++'s `-fpermissive`, which
clang lacks), that are x86-only, or that read uninitialised memory (listed in
the file; each patch must match exactly once, so an upstream change fails
loudly).

## Rebuilding from source

```
sudo apt install python3 g++ libgtk-3-dev libsdl2-dev
python3 build.py            # uses ./Furbtendulator-src, writes build/furb, build/furb_cli, build/Mappers/
python3 selftest.py         # checks furb_cli
python3 tools/gui_smoke.py  # checks furb (needs xvfb xdotool openbox)
```

`--no-gui` builds `furb_cli` only (no GTK/SDL needed); `--m32` builds for
32-bit x86 (`g++-multilib`, and i386 GTK/SDL development packages for the
GUI); `--touch` also builds `furb_touch` (above); `--cxx clang++ --cc clang
--objcopy llvm-objcopy` builds with clang, and `--cflags` adds flags such as
`--target=...` for cross builds; `--android ABI` builds the Android libraries
(`tools/build_android.sh` runs it).  `tools/build_bin32.sh` rebuilds `bin32/`: it downloads Ubuntu 20.04's
GCC 9, glibc and i386 GTK 3 / SDL 2 development packages with apt-get into
`build32/` (about 230 MB; nothing is installed), builds both programs against
them with `--sysroot`, and refuses the result if it needs a glibc newer than
2.29.  The first build takes a few minutes and later builds are incremental.
The source file lists come from Furbtendulator's own Visual Studio project
files, so a newer Furbtendulator drops in: point `--furb` at its `src`
directory.  `build.py` never modifies the source tree; it copies it to
`build/src` and adjusts only that copy.

## What is in the package

| Path | Contents |
|------|----------|
| `bin/` | the prebuilt programs (`furb`, `furb_cli`), mapper packs and data files (x86_64, built on Ubuntu 24.04, GCC 13) |
| `bin-android/` | `Furbtendulator.apk`, the Android app for arm64-v8a, armeabi-v7a, x86 and x86_64 (Android 5.0+, built with NDK r27 and SDL 2.32.10) |
| `bin32/` | standalone 32-bit `furb` and `furb_cli` with their mapper packs and data files, for older systems (i386, glibc 2.29+, built against Ubuntu 20.04's GCC 9, glibc, GTK 3 and SDL 2) |
| `Furbtendulator-src/` | Furbtendulator's complete source as released (minus Visual Studio's IntelliSense cache), plus `bin-data/` (the release's `.cfg` files and empty `BIOS/`, `samples/` folders) |
| `furb_cli.cpp` | the command-line front end (replaces `WinMain`) |
| `compat/` | the Win32/DirectX compatibility layer, `furb_cli`'s host, and the bundled musl math (`compat/libm`) |
| `gui/` | the GUI's Win32 layer on GTK 3 / SDL 2, and `rc2cpp.py`, the resource compiler |
| `android/` | the Android app: `native/` the front end on SDL 2 (also `furb_touch`), `app/` the Java activity, manifest and resources, and the Gradle project |
| `exports.list` | the executables' one exported symbol |
| `build.py`, `prep_src.py` | build driver and source-copy fixups |
| `selftest.py` | `furb_cli` feature self-test |
| `tools/` | `compare_builds.py`, `fuzzrom.py`, `gui_smoke.py`, `touch_smoke.py`, `pacing_sim.cpp`, `build_bin32.sh`, `build_android.sh` |
| `LICENSE` | GNU GPL v2, Furbtendulator's license |

## License and credits

Furbtendulator is Copyright (C) 2023-24 Furbland. It is based on
NintendulatorNRS, Copyright (C) 2017-2023 NewRisingSun, which is based on
Nintendulator, Copyright (C) 2002-2019 QMT Productions. All of them are
licensed under the GNU General Public License, version 2 or later; see
`LICENSE`. The front ends, the compatibility layer, the GUI layer and the
build scripts were written for PocketVT and are distributed under the same
license as a derivative work.  The Android app uses SDL 2 (zlib license),
fetched when it is built, and Apache Commons Compress with XZ for Java
(Apache 2.0 and public domain) for 7z archives.  `compat/libm` is a subset of musl 1.2.4,
Copyright (C) 2005-2020 Rich Felker et al., under the MIT license
(`compat/libm/COPYRIGHT.musl`).  This package includes the complete
corresponding source code for the included binaries.
