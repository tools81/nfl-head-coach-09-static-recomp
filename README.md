# NFL Head Coach 09 — static recompilation

Version 0.7.2

A native Windows port of NFL Head Coach 09 (Xbox 360, title ID `45410841`), built by
statically recompiling the game's PowerPC executable to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and running it on a host layer
that replaces the Xbox kernel, XAM, graphics, audio and input.

No game data is included. You need your own copy of the disc, and nothing under
`iso/`, `build/` or `ppc/` may be redistributed.

## Installing

Download `HC09Setup.exe` from the [releases page](https://github.com/tools81/nfl-head-coach-09-static-recomp/releases)
and run it. The wizard:

1. checks for the build tools: Visual Studio 2022 or later (Community is fine) with *Desktop development
   with C++*, *C++ Clang Compiler for Windows* and *C++ CMake tools for Windows*, and
   [Git for Windows](https://git-scm.com/download/win);
2. asks for an install folder (a short path such as `C:\Games\HC09Recomp` is best) and your
   NFL Head Coach 09 disc image (`.iso`), which it checks is the right game;
3. offers **automatic update checks**: when enabled, the game checks this repository's latest
   release each time it starts and, if there is a newer one, asks before downloading and rebuilding;
4. downloads the source, XenonRecomp and FFmpeg, imports the game's executable from your disc
   image, recompiles it and builds `HC09.exe` (about 1 GB of downloads and 10 to 20 minutes), then
   adds a Start menu shortcut.

The game is built on your PC because the recompiled executable is derived from the game's own code,
which this project does not distribute. The disc image stays where it is: the game reads its data
from it while you play. Settings live in `hc09.ini` in the install folder (`IsoPath`, `AutoUpdate`).
Running setup again and choosing the same folder updates that installation to the latest release
(keeping your saves), and is also how to change the disc image; a development clone on a git branch
is built as it is instead. The setup log is `%TEMP%\HC09Setup.log`.

## Status

| Stage | State |
|---|---|
| Disc extraction, XEX decryption/decompression | done |
| Recompiler config (ABI helpers, setjmp/longjmp, jump tables, leaf-function bounds, data skips) | done — recompiler log is clean |
| Missing PPC/VMX instructions in XenonRecomp (~40 opcodes), guarded integer division | implemented in `patches/`, not yet verified against hardware tests |
| Host executable builds and links | done |
| Memory, Rtl, TLS, time, strings/printf, XEX header, kernel variables | implemented |
| Threads, waits, events, semaphores, mutants, timers | implemented (no APCs, no forced suspend) |
| File system (disc read straight from the ISO, writable `build/userdata`) | implemented |
| GPU command processor (ring buffer, fences, scratch write-back, per-CPU interrupts, vblank, swaps, shader/constant loads, predicated tiling) | implemented |
| Physical memory windows (0xA0000000 / 0xC0000000 / 0xE0000000 aliased) | implemented (0xE0000000's 4 KiB offset not reproduced) |
| Xenos shader microcode → HLSL translator | implemented; all 148 shaders captured from menus and gameplay translate and compile. Compiled with DXC (shader model 6.0, `dxcompiler.dll` and `dxil.dll` copied from the Windows SDK at build time) in the background and cached in `build/shadercache`: the 148 take about 5 s in all, at most 0.12 s each, against 52 s and up to 6 s with D3DCompile, which is used when DXC is missing or the GPU lacks shader model 6.0 |
| D3D12 renderer | draws, depth/stencil, culling, blending, alpha test, textures (linear/tiled, full mip chains incl. packed mip tails, render-to-texture mips, anisotropic filtering, border colours, cube maps, volumes, stacked arrays with normalized layer coordinates, common formats, resolved depth), resolves (incl. tiled colour and depth strips and the red/blue copy swap), present, fast clears through multisampled EDRAM aliases, 4x MSAA for the multisampled surfaces the 3D scenes render to (`HC09_NO_MSAA=1` turns it off) |
| Audio output | WASAPI shared mode, 5.1 mix downmixed to stereo |
| XMA decoder | FFmpeg's XMA2 decoder (LGPL build in `thirdparty/ffmpeg`), 16-bit PCM into the title's output rings |
| Music player (XMP) | no user soundtrack: always idle, controls accepted |
| Controller input (XInput) | implemented, plus a keyboard/scripted virtual pad for player 1 |
| Saves (XAM content packages) | implemented: packages are folders under `build/userdata/content/<type>/` |
| Remaining kernel/XAM imports | 36 still stubbed (APCs, voice chat, Live UI, marketplace); none are called in normal play. System message boxes answer with their focused button at once |

The game boots, plays its intro movie in a window, loads, renders the title screen, and
reaches its menus (favourite-team prompt, main menu with its 3D background, team select) with
controller or keyboard input, and plays a game: weekly coaching agenda, coin toss, and the
in-game play-calling screen, where called plays run and the clock and downs advance. Career
Mode (new career, season goals, the career hub, training camp, gameplan, then a preseason game
in another stadium), the Xbox Live, My Coach and Media World menus, and a 21-minute unattended
match (score and quarters advancing, memory flat) all run without errors. Menus and in-game screens run at about 30-35 fps with music and sound effects, and the
profile saves (Quick Save, LS) to `build/userdata/content` and loads on the next start.

## Controls

Player 1 is always connected. A real controller works; the keyboard also works while the game
window has focus: Enter = Start, Esc = Back, Z/Space = A, X = B, C = X, V = Y, Q/E = LB/RB,
1/3 = LT/RT, F/H = left/right stick click, arrows = D-pad, WASD = left stick, IJKL = right stick.

F11 or Alt+Enter toggles borderless fullscreen. The window can be resized freely; the picture
keeps its 16:9 shape, with black bars where the window's shape differs.

`set HC09_RESOLUTION_SCALE=2` (up to 4) renders at that multiple of the game's 1280x720, e.g.
2560x1440, for a sharper picture in fullscreen on a high-resolution display. Menus and gameplay
held 30-35 fps at 2x in testing; 1x is the default.

## Rosters and replacement images

`tools/roster_import` builds a roster from a modern ratings export and current coaching staffs
(see its [README](tools/roster_import/README.md)), with player and coach portraits.

**HC27.exe**, built alongside HC09.exe, is the game with that roster: it reads `hc27\ROSTER` in
place of the disc's default roster (so new careers and the roster screens start with it, nothing
to load), lays the patches in `hc27\disc` (`<archive>@<hex offset>.bin`, such as the league's
finances that `league_salaries.py` writes) over the disc's files, draws the images in
`hc27\textures` before those in `textures`, and keeps its saves apart in `build\userdata-hc27`.
Its seasons start in 2026: the game's calendar still runs from 2008, so every year it shows (the
clipboard's date, the salary cap years, draft classes, important dates) is 18 years later. Without `hc27\ROSTER` it reports what to build and exits.
HC09.exe is unaffected by the `hc27` folder.

The front end's "swappable" images (player and coach portraits, team logos, helmets and the
other `headcoach/swappables/...` libraries) can be replaced without touching the disc: a PNG at
`textures/swappables/<library>/<id>.png` in the install folder (for HC27, also
`hc27/textures/swappables/...`) is drawn instead of image `<id>` of that library, at its own resolution (images more than twice the game's size are halved until
they fit, since these are drawn without mip maps). A player's portrait id is `PSXP` in the
roster. `HC09_NO_REPLACEMENTS=1` draws the game's own images; `HC09_TRACE_SWAPPABLES=1` logs
each image the game loads (`[swappable] PlayerPortraits/1650 -> texture 0x161CF000`).

Any other 2D texture (a logo in a menu, a helmet decal, a sign at the career campus) can be
replaced by its contents: a PNG at `textures/hashed/<content hash>.png` (for HC27, also
`hc27/textures/hashed/...`) is drawn wherever the game uses that texture, with mip maps built for
it. `HC09_DUMP_HASHED=1` saves every texture the game draws, once, as
`build/hashed/<content hash>.png`, in the colours it is drawn with, so the one to replace can be
found and edited; the log names the swappable image a dumped texture holds, if any. Both kinds of
replacement image hold the colours as drawn (a texture's channel order does not matter).

HC27 can also rename a team (`TEAM_RENAMES` in `tools/roster_import/import_roster.py`; the
Redskins are the Commanders): the game names a team's uniforms, helmets and career campus after
its name in the roster and finds them by a hash of that name, so HC27 hashes the old name for
them (`kRenamedTeams` in `src/common/edition.cpp`).

## Known issues

- Rendering: real MSAA only for multisampled surfaces used by predicated tiling (the 3D
  scenes); other multisampled surfaces render single-sampled. EDRAM aliasing is reproduced only
  for clear rectangles drawn into a multisampled surface that covers a single-sampled one.
  Alpha-to-coverage (`RB_COLORCONTROL` bit 4) is not implemented; the title does not enable it
  in the menus or in gameplay.
- Predicated tiling is approximated for screen-sized targets: the first tile pass renders the
  whole frame, later passes skip their draws, and tile clears are skipped (the per-draw bin
  masks the GPU derives from screen-extent queries are not reproduced).
- The video board and LED ribbon boards at the Colts' stadium (Lucas Oil) show a flat, flickering
  white/cyan slab. This is the game's own data, not the port: the `JUMBOTRON` effects reference
  source-art paths that were never exported (`.../textures/stadiums/scoreboards/jumbotron_COLOR.dds`,
  `jumbo_GRIME_color4.dds`, `jumbo_ads_color_COL`, ...), so the engine substitutes its grey
  `Missing.dds` for the LED pixel and grime masks, and the shader's 9x brightness then saturates the
  board. `HC09_ENGINE_LOG=1` shows the engine's own "Asset missing, using default" messages for
  them. Stadiums whose boards use other materials (e.g. Texas Stadium) render normally.

## Requirements

- Visual Studio 2022 with *Desktop development with C++*, *C++ Clang Compiler for Windows* and *MSBuild support for LLVM (clang-cl)*
- CMake ≥ 3.20 and Ninja
- Python 3.12 (a project venv is used: `python -m venv .venv` then `.venv\Scripts\pip install pycryptodome capstone`)
- FFmpeg 8.1 LGPL shared build (BtbN `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`) extracted to
  `thirdparty/ffmpeg` (provides `include/`, `lib/` and the DLLs copied next to `HC09.exe`)

## Pipeline

Run from the repository root. Put the disc image at `iso/NFL Head Coach 09.iso`.

```sh
# 1. Build XenonRecomp with the HC09 instruction patch
git clone --recursive https://github.com/hedge-dev/XenonRecomp thirdparty/XenonRecomp
git -C thirdparty/XenonRecomp checkout ddd128b
git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp.patch
tools\build_xenonrecomp.bat

# 2. Extract and decrypt the executable (the XEX key is read from the XenonRecomp checkout)
python -I tools/xdvdfs.py extract "iso/NFL Head Coach 09.iso" build/extracted default.xex
.venv/Scripts/python -I tools/xexdecrypt.py build/extracted/default.xex build/default.pe
.venv/Scripts/python -I tools/xeximports.py build/extracted/default.xex build/default.pe --csv build/imports.csv

# 3. Analyse and generate the recompiler config
build/xenonrecomp/XenonAnalyse/XenonAnalyse.exe build/extracted/default.xex config/HC09_switch_tables.toml
.venv/Scripts/python -I tools/gen_functions.py build/default.pe config/HC09_switch_tables.toml config/HC09_functions.toml build/imports.csv
# Jump tables XenonAnalyse misses (dispatches without a bounds check): check, then add
.venv/Scripts/python -I tools/check_switches.py build/default.pe config/HC09_switch_tables.toml
.venv/Scripts/python -I tools/add_missing_switches.py build/default.pe config/HC09_switch_tables.toml 0x824374DC 0x8254D164 0x82596A14 0x825B1874 0x82E575D4
.venv/Scripts/python -I tools/make_config.py

# 4. Recompile to C++ (into ppc/)
mkdir ppc
build/xenonrecomp/XenonRecomp/XenonRecomp.exe config/HC09.toml thirdparty/XenonRecomp/XenonUtils/ppc_context.h

# 5. Build and run
.venv/Scripts/python -I tools/gen_import_stubs.py build/imports.csv
tools\build.bat
build\hc09\HC09.exe
```

`HC09.exe` finds the project folder itself (it looks upward from its own location for
`build\default.pe`), so it can be double-clicked in `build\hc09` or started from a shortcut.
If something it needs is missing, it says so in a message box. It takes the disc image from its
command line (`HC09.exe [image.pe] [default.xex] [disc.iso]`), else from `hc09.ini`, else from
`iso/NFL Head Coach 09.iso`.

The XenonRecomp commit is pinned in `patches/xenonrecomp.commit` (setup reads it from there).
`build\installer\HC09Setup.exe --import <disc.iso> <project folder>` does step 2's extraction and
decryption without Python (the configs and `src/kernel/import_slots.inc` from steps 2 and 3 are
committed, so the setup wizard skips those steps).

### Releases

Setup and the in-game update check use the repository's latest GitHub release: tag it `v<version>`
with the version in `CMakeLists.txt` (`project(HC09Recomp VERSION ...)`), and attach
`HC09Setup.exe`, which contains no game code. Build it standalone with either compiler:

```sh
cmake -S tools/installer -B build/setup -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/setup    # -> build/installer/HC09Setup.exe
```

Installed copies offer the update the next time they start; accepting it checks out the release
tag, re-patches XenonRecomp if the pin changed, and recompiles and rebuilds the game.

## Layout

| Path | Contents |
|---|---|
| `config/HC09.base.toml` | Hand-written recompiler settings (edit this one) |
| `config/HC09.toml` | Generated: base + `HC09_functions.toml` |
| `config/HC09_switch_tables.toml` | Generated by XenonAnalyse |
| `patches/` | XenonRecomp changes: new instructions, `bdnzf` fix |
| `src/` | Host runtime: memory map, thread block, kernel/XAM imports |
| `src/common/` | Shared by the game and setup: `hc09.ini` settings, GitHub release check |
| `tools/installer/` | `HC09Setup.exe`: install/update wizard (disc import, build pipeline) |
| `tools/` | Extraction, decryption and analysis scripts |

## Debugging

- Unimplemented imports log their first call: `[stub] Name (library) called from 0x...`.
- Access violations inside guest memory print the faulting guest address.
- `set HC09_DUMP_STACKS=20` dumps every guest thread's call stack after 20 seconds
  (`20,30` dumps at each time).
  Recompiled functions are named `sub_<guest address>`, so these read as guest stacks.
- `set HC09_WATCH=830DD0E1` puts a hardware write breakpoint on guest bytes (up to 4,
  comma-separated) and logs each write with the writer's stack. `*83229400+164` watches the
  8 bytes at (pointer stored at 0x83229400) + 0x164, resolved 5 s after startup.
  `A6713ED4:4` watches 4 bytes (1, 2, 4 or 8, aligned); `HC09_WATCH_VALUE=<hex>` reports only
  writes that leave that value and `HC09_WATCH_AFTER=<seconds>` arms the watches that late. Each
  distinct writer stack is printed once.
- `set HC09_ENGINE_LOG=1` prints the engine's own debug messages (compiled out in retail) as
  `[engine] ...`, e.g. `Asset missing, using default: <path>` for every texture a material
  references that no loaded package provides.
- `set HC09_SAMPLE_THREAD=160` samples guest thread 0x160's stack every 2 ms for its whole
  life and prints each distinct call path once.
- Indirect calls to code that was not recompiled as a function stop with
  `indirect call to 0x..., which is not a recompiled function`; access violations print
  the faulting stack with `ppc/*.cpp` line numbers.
- `src/debug/hooks.cpp` overrides individual recompiled functions (`sub_<address>`) to log
  or patch them; the original stays callable as `__imp__sub_<address>`.
- `set HC09_GPU_STATS=1` prints command-processor throughput, packet counts and its time split
  (draw / swap / idle) every 2 s, plus the share of wall time in each phase of a draw and
  texture-cache counters, and a `[gpu] memory` line: video memory in use, cached textures and
  resolve targets (with how many were released after a minute unused), resolves dropped as stale
  (the title loaded other data over their memory) and fetches a resolve could not serve.
- `set HC09_USERDATA=C:\path\folder` keeps saves and the cache in another folder (e.g. a fresh
  profile for `HC09_PRESS` scripts, which assume the first-start prompts).
- `set HC09_DUMP_FRAME=300` traces every packet and draw of frame 300 (`13500/150`: frame 13500 and every 150th after it);
  `HC09_FIND_DWORDS=<hex,...>` searches guest memory for a dword sequence at that frame and
  `HC09_FIND_STRING=<text>` for that text (case-insensitive), printing the surrounding characters.
  Frame dumps also log each draw's shader bool constants (`[frame]   bools`).
- `set HC09_DUMP_SHADERS=1` writes each new shader's microcode to `build/shaders/`;
  `build\hc09\XenosDisasm.exe [--hlsl] build\shaders\*.bin` disassembles, translates and
  compiles them.
- `set HC09_SCREENSHOT=300` saves frame 300 as `build/screenshot.bmp`
  (`python tools/bmp_to_png.py build/screenshot.bmp out.png`).
- `set HC09_TRACE_NAMES=1` (HC27) prints each distinct asset name the game looks up in its archives
  (packages, models, textures), with the caller.
- `set HC09_FXC=1` compiles shaders with D3DCompile instead of DXC; `HC09_SYNC_SHADERS=1` compiles each
  when first drawn instead of in the background (with `HC09_GPU_STATS=1`, printing how long each took).
- `set HC09_D3D12_DEBUG=1` enables the D3D12 debug layer and prints each distinct message; `HC09_NO_RENDER=1` disables rendering.
- `set HC09_AUDIO_RECORD=C:\path\out.wav` records everything sent to the speakers (48 kHz stereo).
- `set HC09_AUDIO_STATS=1` prints XMA decoder counters, the output peak level and the sound card
  queue depth (normally about 25 ms) every 5 s;
  `HC09_NO_AUDIO=1` disables audio output; `HC09_XMA_DUMP=1` saves one context's packets to
  `build/xma_dump.bin` for `build\hc09\XmaTest.exe`.
- `set HC09_PRESS=1800:start,1900:a` presses buttons at those frames (held 6 frames).
- `set HC09_STACK_ON_OPEN=campus` prints the call stack of every file open whose path contains
  that text.
- `set HC09_DRAW_SNAPSHOTS=1` with `HC09_DUMP_FRAME` saves the render target after every
  visible draw of that frame (`build/draw_<n>.bmp`); `HC09_DUMP_TEXTURES=<frame>` saves the
  RGBA8 textures sampled from that frame on (`HC09_DUMP_TEXTURES=dump`: those sampled in the dump
  frame). `HC09_DRAW_SNAPSHOTS=<first>-<last>` limits the snapshots to those draws and
  `HC09_DRAW_SNAPSHOTS=s<RB_SURFACE_INFO hex>` to draws to that surface; `HC09_STENCIL_SNAPSHOTS=1` also saves the stencil plane
  (`build/stencil_<n>.bmp`: bit 0 red, bit 1 green). Snapshot `<n>` shows the result of the
  draw logged as `<n-1>`.
- `set HC09_DUMP_AFTER_RESOLVE=512x512+2@2500` dumps (as `HC09_DUMP_FRAME` would) the frame
  2 frames after the first 512x512 colour resolve at or after frame 2500, for events whose
  frame number varies between runs. `HC09_TRACE_RESOLVES=1` logs every colour resolve;
  `HC09_SAVE_RESOLVES=512x512` saves each resolve of that size as `build/resolve_<frame>_<addr>.bmp`
  (`512x512@2500-2510` only in those frames).
- `set HC09_DUMP_VS_CONSTS=152-155` / `HC09_DUMP_PS_CONSTS=200-255` (with a frame dump) print
  that range of vertex or pixel shader constants for every draw. Frame dumps also log each
  draw's blend/depth/stencil registers, its index range and every bound texture (format, size,
  tiling, swizzle); `HC09_DUMP_VERTEX_DATA=1` adds the first 48 dwords of every vertex buffer
  (slow: it also prints texture slots read as vertex fetches).
- `set HC09_TRACE_SURFACE=04020118` logs every draw to that render surface (`RB_SURFACE_INFO`) in
  any frame, with its index/depth/clip registers, and saves the target and depth buffer after each
  one (`build/surface_<n>.bmp`, `build/surface_<n>_depth.bmp`). Debugging overrides:
  `HC09_SAMPLER_TEST=lod0|wrap|point`, `HC09_NO_DEPTH_TEST=1`, `HC09_FORCE_INDEXED=1`.
- With a frame dump, `HC09_DUMP_TEXTURE_MEMORY=1` saves every sampled texture's raw guest memory
  (`build/texmem_<frame>_<base>_f<format>_<w>x<h>_p<pitch>.bin`), `HC09_DUMP_VERTEX_MEMORY=1` every vertex
  buffer read (endian-swapped, `build/vbmem_<address>_s<stride>.bin`), and
  `HC09_PROBE_PIXEL=<x>,<y>` logs which draws change that render-target pixel (`[probe]`).
- `set HC09_TRACE_RING=<seconds>` logs ring-buffer batches from that time;
  `HC09_TRACE_FENCES=<hex>` logs fence writes (and ring indirect-buffer calls) once a fence
  value reaches that number; `HC09_TRACE_D3D=1` reports long D3D fence waits (with the
  D3D worker queues) and concurrent kickoffs; `HC09_TRACE_KICKS=<seconds>` logs ring kicks;
  `HC09_TRACE_WAITS=1` logs each distinct `WAIT_REG_MEM`. Critical-section waits over 3 s
  are always logged (`[cs]`). `HC09_THREAD_STATS=1` prints each thread's CPU share every 5 s
  (the command processor is 0xC0, the XMA decoder 0xA0).
- `set HC09_DUMP_MEMORY=1AD1D000:8000` (with `HC09_DUMP_FRAME`) saves raw physical memory
  as `build/mem_<addr>.bin`; `HC09_NO_E_WINDOW=1` leaves the 0xE0000000 view unmapped.
- `set HC09_DUMP_MIPS=256x256f2` saves every mip level of textures of that size and Xenos format,
  as stored by the game (untiled, byte-swapped), to `build/mip_<frame>_<base>_<level>_<w>x<h>.bin` (frame of the upload).
  `HC09_ORIGINAL_MIPS=1` keeps the stored mip levels of textures whose levels are not downscales of
  level 0 (such as the field's grass noise); by default those are rebuilt from level 0 (`[gpu] texture ... rebuilding them`).

## Analysis tools

- `tools/peinfo.py` — sections and `.pdata` stats
- `tools/find_abi.py` — locate `__savegprlr_14` and friends
- `tools/xrefs.py` — find branches to an address
- `tools/disasm.py` — disassemble a range
- `tools/check_switches.py` — cross-check jump-table coverage
