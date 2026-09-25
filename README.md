# ps2recomp: Ace Combat 5

**简体中文说明见 [`README.zh-CN.md`](README.zh-CN.md)。**

A static recompilation of **Ace Combat 5: The Unsung War** (PS2) for Windows. It builds for both the US release and the Japanese / Chinese one, on x64 and on Windows on ARM.

> This fork adds the Japanese / Chinese region, a Windows-on-ARM build, and the
> troubleshooting that got the Chinese localisation from a black screen to the main
> menu. **It was modified by DeepSeek Harness driving the DeepSeek V4.1 Flash
> model.** Upstream: [sal063/Ace-Combat-5-Static-recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation).

The game's main CPU code isn't emulated. A Python tool reads the original executable and translates every function into C ahead of time. GCC then compiles that together with a runtime that stands in for the rest of the console: the GS (drawn through Vulkan), the VU vector units, SPU2 audio, the IPU for the movies, the IOP modules, memory cards and controllers. What you get at the end is a normal `ac5.exe`.

The graphics are native too. The 3D (the aircraft, the cockpit, terrain, ground objects, the sky and clouds) doesn't go through an emulated Graphics Synthesizer. The vector programs the game runs on the VU1 to transform and light its models have been rewritten as native code, and the geometry is drawn as real GPU meshes, with vertex shaders, mipmapped textures and anisotropic filtering. The flight HUD, the radar and the radio captions are drawn at your window's resolution, so they stay sharp at any size. Whatever the native renderer doesn't cover yet (the menus, the hangar, some effects) still goes through the emulated GS, into the same frame.

The game is fully playable.

There's no game code or assets in this repo. You bring your own copy of the game and the recompiler builds the C code from it on your machine, which is also why `generated/` is in `.gitignore`.

## What you need

- **The game**, in either of two releases:
  - the US release, serial **SLUS-20851** (`SLUS_208.51` on the disc)
  - the Japanese release, serial **SLPS-25418** (`SLPS_254.18`), which is also what the Chinese localisation patches — so a Chinese-translated image is built with the same steps and the same config

  Either an ISO or the extracted disc files. The addresses in the config files are per-region, and `config/` holds the US set while `config/cnjp/` holds the Japanese one, already translated from the US set. See [The Chinese / Japanese build](#the-chinese--japanese-build).
- **64-bit Windows** and a GPU with a Vulkan driver. I build and play on Windows 10.
- **[MSYS2](https://www.msys2.org)**, for GCC and SDL3. In the MSYS2 UCRT64 shell run:

  ```
  pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-pkgconf
  ```

  It has to be GCC 15 or newer. The generated code relies on guaranteed tail calls (`[[gnu::musttail]]`), and with an older compiler you get a CMake warning and an exe that can run out of stack.
- **CMake** 3.20 or newer, and **Ninja** (`pip install ninja` is the easiest way to get it).
- **The [Vulkan SDK](https://vulkan.lunarg.com)**. The build uses its `glslc` to compile the shaders.
- **Python 3.** Only the standard library is used, there's nothing to pip install. I'm on 3.12.

## Step 1: get the executable off the disc

The recompiler only needs one file from the disc, `SLUS_208.51`, which is the game's main executable. It's in the root of the ISO. Mount the ISO in Explorer (double-click it) or open it with 7-Zip, and copy that file somewhere.

Make sure it's the right one before going any further:

```powershell
Get-FileHash .\SLUS_208.51 -Algorithm SHA256
```

You should get:

```
C3594227605307806592416DBD723BF5771952E279EE281A40DA305426667385
```

The file should also be exactly 3,634,092 bytes. If the hash doesn't match, stop here. You've got a different region or a modified executable, and the recompile won't line up with the config files.

Keep the ISO around. The executable is just the code, the game still loads its models, textures, sound and movies off the disc while you play.

## Step 2: set up a terminal

Everything from here on is PowerShell, run from the root of this repo. Put MSYS2's UCRT64 `bin` folder at the front of your PATH first, so CMake can find `gcc` and `pkg-config`:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
```

That only applies to the window you're in. Change the path if MSYS2 isn't installed in `C:\msys64`. If you only just installed the Vulkan SDK, open a new terminal so it picks up the `VULKAN_SDK` variable.

## Step 3: recompile

```powershell
$env:PYTHONPATH = "tools"
python -m ps2recomp "C:\path\to\SLUS_208.51" -o generated `
    --ida-db config/ida_db.json `
    --ida-seeds config/ida_seeds.json `
    --symbols config/sdk_symbols.json `
    --symbols config/manual_symbols.json `
    --overrides config/overrides.json `
    --hooks config/hooks.json
```

This takes about 15 seconds, and the last line should be:

```
emitted 10742 functions into 43 files in 15.0s
```

(the time will be different for you). That gives you a `generated/` folder of about 50 MB:

- `ps2_code_0000.c` through `ps2_code_0042.c`: the recompiled functions
- `ps2_func_table.c` and `ps2_funcs.h`: the address to function table the runtime dispatches through
- `ps2_symbols.c`: names for the functions that have one
- `ps2_image.bin`: the executable's memory image, which the runtime loads at startup (`ps2_image.c` just records where it goes and how big it is)

Use all of those flags. There's a shorter version with only `--ida-db` sitting in an error message in `CMakeLists.txt`, don't go by that one. It does produce code that compiles, but it leaves out the overrides and hooks, and the game won't work without them. The overrides replace SDK functions that talk to hardware the PC doesn't have (SIF RPC, the CD drive, the pads and so on) with native handlers in `runtime/src/ps2_hle_*.c`. Recompile those faithfully and the game just sits there waiting on hardware that never answers.

The rest, in short:

- `ida_db.json` and `ida_seeds.json` are function boundaries and entry points exported from IDA. When I tested the recompiler on the SDK sample programs, it found roughly two thirds of the functions on its own and about 97% with the IDA data.
- `sdk_symbols.json` names the PS2 SDK functions linked into the game, found by signature matching. `manual_symbols.json` is the handful the matcher couldn't place.
- `hooks.json` hangs native handlers off a few of the game's own functions (scene changes, file opens, sound loading, the radio queue). They run first, then the original code carries on as normal.

If you want to actually read the output, add `--comments`. Every line then gets the original MIPS instruction written next to it. The files come out about half again as big, but the code is exactly the same.

## Step 4: build

```powershell
cmake -S . -B build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
    -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/c++.exe
cmake --build build/gcc
```

The shaders get compiled into a `shaders` folder right next to `ac5.exe`, and that's where the game looks for them. If you ever move the exe somewhere else, take that folder with it.

Give it some time. Every generated file is basically one gigantic function and GCC takes its time with them. It takes me 3 to 5 minutes on a 12-thread CPU. There shouldn't be any warnings.

When it's done you'll have:

- `build/gcc/ac5.exe`: the game
- `build/gcc/shaders/`: the compiled shaders
- `build/gcc/gsreplay.exe`: a dev tool that replays graphics captures, not needed to play

## Step 5: play

Still in the repo root:

```powershell
.\build\gcc\ac5.exe --data generated --disc "C:\path\to\Ace Combat 5 - The Unsung War (USA) (En,Ja).iso" --watchdog 0
```

- `--data` is the folder that has `ps2_image.bin` in it. That is the recompiler's output directory — `generated` or `generated-cnjp` — **not** the disc. Get it wrong and the run stops before the guest starts, printing the full path of every place it looked. Leaving `--data` off makes it search `generated-cnjp/`, `generated/`, `out/generated/`, `..` and `.`, which covers the usual layouts.
- `--disc` takes the ISO or a folder with the extracted disc.
- `--watchdog 0` switches the watchdog off. Normally the runtime gives up if the game hasn't delivered a frame in 10 seconds. That's useful when something's hung during debugging, not so much when you're playing.

The window stays black for about 20 seconds before the first picture. That's normal, give it a moment. The log goes to stderr. In PowerShell 7 you can save it by sticking `2> ac5_log.txt` on the end. The older Windows PowerShell 5.1 mangles stderr when you redirect it like that, so if that's what you have, run the same command from `cmd` instead. `--verbose` makes it log a lot more (and it really is a lot).

If you start `ac5.exe` from anywhere other than that terminal (double-clicking it in Explorer, say), Windows won't be able to find SDL3 and pthread. The build now copies both DLLs next to `ac5.exe` for you, out of `runtime/dll/<arch>/`, so it starts from anywhere and there is nothing to install by hand. `vulkan-1.dll` comes from your graphics driver and is deliberately not bundled. Keep in mind it still needs `--data` and `--disc`, so a shortcut with those arguments filled in is the easiest way to launch it outside a terminal.

Where your stuff goes:

- Saves are written to a `saves` folder inside whatever folder you started the game from. The memory card files get created the first time the game touches them. Set `PS2_SAVE_DIR` if you want them somewhere else. These are this runtime's own format, not PCSX2 memory cards, so you can't bring saves over from an emulator.
- Settings are saved to `ac5_settings.ini` next to `ac5.exe`.
- Compiled graphics pipelines are cached in `ac5_pipelines.cache` and `ac5_pipelines.keys` next to `ac5.exe`, so a state that was compiled once doesn't cause a stutter the next time it shows up. Deleting them is harmless, they just get rebuilt.

### Controls

Controllers go through SDL, so anything SDL recognizes as a gamepad should just work. Buttons map by position: the bottom face button is Cross, right is Circle, left is Square, top is Triangle. Bumpers are L1/R1, triggers are L2/R2, clicking the sticks gives L3/R3, and Back and Start are Select and Start.

Keyboard defaults:

| PS2 | Keyboard |
| --- | --- |
| D-pad | Arrow keys |
| Cross, Circle, Square, Triangle | X, S, Z, A |
| L1, R1 | Q, E |
| L2, R2 | 1, 3 |
| Left stick | Numpad 8, 4, 2, 6 (W also works for up) |
| Start, Select | Enter, Right Shift |

You can rebind all of it in the settings menu.

Other keys:

- **F4** opens and closes the settings menu
- **F11** or **Alt+Enter** toggles fullscreen
- **Esc** quits, or closes the settings menu if it's open
- **F6 to F10** are debugging hotkeys (captures and state dumps), you can ignore them

## The Chinese / Japanese build

The Japanese release is a different executable from the US one, with code inserted
and removed throughout, so the US config files do not fit it. Rather than hand-port
them, `tools/cnjp` does it mechanically and reproducibly.

Get the executable off the disc the same way as step 1, but `SLPS_254.18` (3,634,988
bytes, SHA-256 `B510EE45343325BDACF14B81E3A1B34DE103C1F0379BEAA014795A4E401025AD`),
then:

```powershell
$env:PYTHONPATH = "$PWD\tools"

# 1. work out where every US address went
python -m cnjp map --source SLUS_208.51 --target SLPS_254.18 --ida-db config/ida_db.json -o addr_map.json

# 2. rewrite the config files through that map
python -m cnjp port --map addr_map.json --config config --out config/cnjp --region cnjp

# 3. recompile and build
python -m ps2recomp SLPS_254.18 -o generated-cnjp --ida-db config/cnjp/ida_db.json `
  --ida-seeds config/cnjp/ida_seeds.json --symbols config/cnjp/sdk_symbols.json `
  --symbols config/cnjp/manual_symbols.json --overrides config/cnjp/overrides.json `
  --hooks config/cnjp/hooks.json --report config/cnjp/report.json

cmake -S . -B build/cnjp -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/c++.exe `
  -DPS2_GENERATED_DIR=generated-cnjp
cmake --build build/cnjp
```

`config/cnjp/` is committed, so step 3 is all you need if you are not changing the
port itself.

### One command instead
`tools/build_all.ps1` wraps the whole chain — extracting the executable, the
recompile, and the build — for any or all of the three targets:

```powershell
pwsh -File tools/build_all.ps1                  # all three: CN/JP x64, US x64, CN/JP arm64
pwsh -File tools/build_all.ps1 -What cnjp       # just the Japanese / Chinese x64 build
pwsh -File tools/build_all.ps1 -What us         # just the US build
pwsh -File tools/build_all.ps1 -What arm64      # just the arm64 cross build
pwsh -File tools/build_all.ps1 -Recompile       # redo the recompiler step too
pwsh -File tools/build_all.ps1 -Why             # report what is missing, then stop
```

It checks the toolchain first and says what is absent instead of failing halfway
through, extracts `SLUS_208.51` / `SLPS_254.18` from the disc image if you have not
already, fetches the arm64 cross toolchain when that target is wanted, and picks the
right per-architecture DLLs. Run `-Why` first on a fresh machine.

### Packaging a standalone folder

```powershell
pwsh -File tools/export_release.ps1 -Region cnjp -PersonalUseOnly
```

produces `dist/ac5-cnjp-x64/` — the exe, its DLLs, the shaders, the recompiler's
memory image and a launcher — which can be copied to any Windows machine and
started by double-clicking `launch.cmd`. Whoever runs it supplies their own disc
image; the launcher finds one or asks for it, checks that it is a full-size image,
and refuses to start an arm64 build on an x64 host.

The package includes `ps2_image.bin` because the program will not start without it:
launched without it, the game stalls in its first second and the window stays black
rather than reporting anything. That file is also a copy of the game's own
executable, so the script refuses to run without `-PersonalUseOnly` and `dist/` is
ignored by git. To share a build publicly, share the source and have people
recompile — that produces the image on their own machine.

### How the config was translated

The two executables share long runs of byte-identical code, just at different
addresses, so `cnjp map` walks both `.text` sections and records, for each stretch,
the constant offset between them. Those offsets are piecewise — the port measures
26,388 separate intervals, and the delta is **not** one number: in the regions that
matter it is −0x28, +0x0, +0x8, +0x2B0 or +0x328 depending on where you are.

Every function in the US export is then checked against the target:

| | |
| --- | --- |
| US functions checked | 6,767 |
| translated and verified byte-for-byte | 6,622 (97.9%) |
| recovered by searching around the predicted address | 50 |
| matched on opcodes alone | 81 |
| not locatable | 14 |
| US recompile | 10,742 functions, 98.04% of `.text` |
| Chinese / Japanese recompile | 10,670 functions, 97.6% of `.text` |

Two of those rows are worth understanding, because both were bugs I had to fix
after seeing them fail at run time:

- **"matched on opcodes alone"** exists because comparing whole instructions is too
  strict. The same function in the two builds can reorder a run of
  `addiu reg, reg, N` and still do the same work, so the comparison drops every
  immediate and keeps only the opcode and register fields. A relaxed prefix is weak
  evidence on its own, so a candidate also has to be a function of comparable
  length, must not already be claimed by another function, and must keep the
  reference's function order. Without those three checks the search mapped several
  short functions onto one long one — six reference functions landing on a single
  target in one case — and the recompiled build contained garbage there.
- **"not locatable"** functions are ones the two builds genuinely do not share.
  Their address ranges are left out of the seed list, so the recompiler recovers
  that code from the call graph instead of trusting a boundary that would be wrong.

### Region-independent addressing in the runtime

Anything in the runtime that names a guest address has to find it again on the
other region, and there is no single shift that works. Addresses are resolved at
start-up by scanning the loaded executable for the function, not by arithmetic:

- `rn_tap.c` (the native renderer's tap points) carries a signature per entry —
  instruction values plus a per-word mask — and resolves them by matching.
- `rn_intent.c` names about forty more functions. These fall into five shift
  regions. Resolving them one at a time does not work: with the immediate fields
  masked, their `addiu sp, sp, -N` prologues match over a thousand places. So each
  region is resolved as a whole — its first entry is scanned for, a unique hit gives
  the region's shift, and the shift is then applied to every member and re-checked
  against its recorded instructions. `intent_addrs[]` and `intent_region[]` are
  generated together, because a single misplaced region index resolves the wrong
  functions.
- Verification reads the **loaded executable image**, not guest RAM: the guest pages
  its own overlays over parts of `.text`, so the primitive writer reads back as
  unrelated code by the time the renderer initialises.

### The one that actually caused the black screen

The build booted, loaded the disc, attached every hook, reported no errors, pumped
vblank at a steady 59.9 Hz — and stayed black. PCSX2 ran the same image to the main
menu, and every function in the diverging path was byte-identical between regions,
so the fault was not in the recompiled code.

It was one hardcoded address. `NUSNDSTR`, the sound-streaming module, keeps a
counter that the guest advances when a sound bank finishes transferring, and the
scene state machine waits for it before moving on:

- US build: the counter is at `0x0047EC9C`, and it counts 1 then 2.
- Japanese build: the counter is at **`0x0047F49C`** — the same variable, `+0x800`.

The runtime polled the US address in both builds. On the Japanese build that
address reads a constant zero, so the game waited forever for a transfer that had
already completed, never left its first screen, and never drew anything. The
address is now resolved by observation: both known addresses are read, and whichever
one is actually moving is the one this build uses.

A smaller one in the same area: the `-fpatchable-function-entry` sled that mod hooks
patch is disabled when building with clang on Windows, because clang's SEH writer
and that flag disagree about prologue sizes and the AArch64 build fails with
`Incorrect size for func_... prologue`.

## Why the picture was still wrong after that

With the black screen fixed, the picture was still broken -- the sky in fragments, large
streaks across the water. The cause was one mistake in five different shapes: **comparing
an address from the running build against a table written in the reference build's
addresses.** Every one of them fails silently, because a failed comparison reports
nothing; the affected part of the renderer just quietly falls back to the emulated path.

Four were address *comparisons*:

| Where | What it covers | What it looked like |
|---|---|---|
| the sky dome's `seg` table | the sky | the dome was built from 32 zero-length segments, so the sky went **black** |
| the frontend range in `rn_2d.c` | menus and UI | frontend draws were treated as world geometry |
| thirteen ranges in `rn_screen.c` | sky passes, sun, clouds, self-shadow, **the whole effect system** | all fell back to the emulated path |
| three constants in `rn_sun.c` | sun flare, lens ghosts, occlusion chain | the same |

The fifth was an address *read*, and the most deceptive of the lot. The renderer gives
the emulated VU1 the game's own microprograms by copying instruction words out of guest
memory, and it read them from the reference build's `.vutext` addresses. The Japanese
executable relocates that whole section by **+0x340**, so it was reading unrelated bytes,
its parse found no upload command, each program reported **zero instruction pairs**, and
every upload was then rejected. **No native VU1 geometry ran at all.** That is why fixing
the composition layer four times changed nothing you could see: the native taps were
receiving no work to compose.

All five now locate their target by **content** rather than assuming a shift -- a
microprogram begins with a `0x60` opcode word, the sky's circle table begins
`0.0, 1.0, sin(pi/16), cos(pi/16)`, and both hold at the right address and not at the
wrong one.

## Translating an address back

All of that needs one capability: turning an address in the running build back into a
reference address. The runtime never had it -- only a handful of forward mappings, one
per name.

The port's whole address map is now compiled in (`tools/gen_revmap.py` produces
`rn_revmap_data.c`: about 25000 intervals covering 97.7% of `.text`). Three things about
it are worth knowing, because each is a trap:

- **It is per-build data.** Compiling one build's table into the other makes that build
  translate its own addresses as if they had moved, which is silently wrong and worse
  than not translating at all. It is generated into each region's output directory, and
  an absent table compiles to an empty one whose lookups return their input -- correct
  for a build where nothing moved.
- **The forward map is not one-to-one.** In 204 zones, two source intervals with
  different deltas land on the same target bytes. The reverse has no answer there, so
  those zones are dropped and the lookups miss, which callers treat as "keep what you
  had". Guessing an answer is the one thing this must not do.
- **The sign is easy to get backwards** and the table still looks entirely plausible:
  writing the delta as `source - target` instead of `target - source` makes every
  translation wrong by twice the offset. It was caught only by checking known pairs.

## Building for Windows on ARM

The same `ac5.exe` builds for AArch64. It needs an x86_64 clang driver to cross
compile, plus an AArch64 sysroot — `C:\msys64\clangarm64\bin\clang.exe` is itself an
AArch64 binary and cannot be used as a cross-compiler on an x64 host, which is the
trap here. `tools/setup_arm64_toolchain.py` fetches both:

```powershell
python tools/setup_arm64_toolchain.py --check
```

Then:

```powershell
cmake -S . -B build/arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-arm64.cmake" `
  -DPS2_HOST_TOOLS="$PWD/work/host-tools" `
  -DPS2_SYSROOT=C:/msys64/clangarm64 `
  -DPS2_GENERATED_DIR=generated-cnjp `
  -DVulkan_LIBRARY=C:/msys64/clangarm64/lib/libvulkan-1.dll.a `
  -DVulkan_INCLUDE_DIR=C:/msys64/clangarm64/include
cmake --build build/arm64
```

Give `CMAKE_TOOLCHAIN_FILE` an absolute path. CMake splits `-D` values on the `.`,
so a relative path arrives as two arguments and the toolchain is silently ignored.

The Vulkan libraries come from the sysroot rather than the LunarG SDK, because that
SDK ships MSVC import libraries that a MinGW/clang toolchain cannot consume.

## Diagnostics

Several behaviours are behind environment variables, all off by default. These are
the ones that actually found bugs:

| Variable | What it does |
| --- | --- |
| `PS2_FB_EVERY=<n>` | writes the GS framebuffer to `out/fb_<field>.ppm` every n fields |
| `PS2_TRACE_CALLS_EVERY=<n>` | dumps the guest call ring every n seconds, plus the IPU state |
| `PS2_TRACE_DISP` | reports whether the display is reading a buffer that was drawn, and how many fields showed nothing current |
| `PS2_WATCH_MEM=0xADDR[:0xEND]` | logs every guest store in a range, with the call ring leading up to it. Needs `-DPS2_DIAG=ON`, because it is a test on every store |

`PS2_TRACE_DISP` plus `PS2_TRACE_CALLS_EVERY` is what separates "nothing was drawn"
from "drawn somewhere the display is not looking", and the memory watch is what
pinned the counter address above.

One diagnostic is always on, because it is cheap and it is the first thing worth
knowing when a build boots but never gets anywhere: whenever the guest asks about
sound-bank transfers, the runtime logs which address the transfer counter is at and
what it reads. A counter stuck at zero while the game is running means the game is
waiting for a sound bank that will never report complete.

## Mods

Mods go in a `mods` folder in whatever folder you start the game from, one folder per mod. Nothing gets repacked or rebuilt. The game reads its files through a layered file system, and a mod is just another layer on top of the disc. That works the same with the ISO and with extracted disc files.

A mod can:

- replace files, including files inside `DATA.PAC`, by dropping them in its `files/` folder under the name the game uses for them
- change the game's named tuning values with a `params.txt`
- apply PCSX2 `.pnach` patches that change data (code patches can't work on recompiled code, and get refused with a message)
- run Lua scripts that hook the game's functions, read and write its memory, react to frames and scene changes, and rewrite the controller input
- run native code from a DLL, but only if its `mod.toml` says `native = true`

`mods/README.md` explains all of it. The Lua API is `runtime/include/ac5mod.h`, and that header stays documented on purpose.

To name files inside `DATA.PAC` you need `config/pac_names.txt`, which is already in the repo. If you want to rebuild it, `python -m modkit.export_names` generates it from `datapack.bin` in the PS4 release. `python -m modkit extract` pulls files out of the archive under the same names, so you have something to start from. Both need `PYTHONPATH` pointing at `tools`.

If the game misbehaves, set `PS2_NO_MODS=1` first. That turns the whole mod layer off, and if the problem is still there, it isn't a mod. `PS2_MOD_DIR` loads mods from a different folder. The log ends with a summary of every mod, conflict, hook and patch.

`tests/mods` has the mods I test the mod layer with. `python tools/make_test_mods.py` adds the ones that need files from your own disc, because those can't be in the repo.

## Optional: recompile the VU1 microprograms as well

The PS2's VU1 runs small vector programs that the game uploads to it while it's running. They aren't in the executable as code, so `ps2recomp` never sees them. The ones the native renderer has replaced never run at all, and by default the runtime interprets the rest. You can record the ones the game really uses and compile those to C too, which is faster for whatever the native renderer doesn't cover yet.

1. Record them. Set the census variable and play like normal:

   ```powershell
   $env:PS2_VU_CENSUS = "1"
   .\build\gcc\ac5.exe --data generated --disc "C:\path\to\game.iso" --watchdog 0
   ```

   Get through the menus and into a mission, because some of the programs only get uploaded once you're actually flying. A short run that never leaves the menus only records the menu programs. Quit with Esc when you're done, and on the way out it writes `out\vu_programs\` (a `manifest.json` plus a `.bin` for each program).

2. Turn them into C. `PYTHONPATH` still needs to point at `tools` for this:

   ```powershell
   Remove-Item Env:PS2_VU_CENSUS
   python -m vurecomp --programs out/vu_programs -o generated/ps2_vu1_progs.inc
   ```

   `python -m vurecomp --programs out/vu_programs --stats` shows what got recorded, if you're curious.

3. Rebuild. There's a catch here: Ninja won't notice the new file on its own, because `ps2_vu.c` only includes it if it exists and nothing depended on it before. A plain `cmake --build` just says "no work to do". Touch `ps2_vu.c` first so it gets recompiled:

   ```powershell
   (Get-Item runtime\src\ps2_vu.c).LastWriteTime = Get-Date
   cmake --build build/gcc
   ```

Next time the game draws something with the VU1, the log should get a line like `vu1: 15 recompiled microprograms available` (15 is what a census through a full mission gave me, yours depends on how far you played). If you ever want to compare against the interpreter, set `PS2_VU_RECOMP=0`.

## Regenerating the config files

You don't need to. Everything in `config/` is already generated and committed, so you don't need IDA or the PS2 SDK to build. If you want to redo them anyway:

- `tools/ida/export_db.py` and `tools/ida/export_seeds.py` are IDA scripts that produce `ida_db.json` and `ida_seeds.json`.
- `tools/identify_sdk.py` rebuilds `sdk_symbols.json` by matching the game against the SDK's EE libraries. Point `PS2SDK_DIR` at your SDK and `AC5_DISC` at the extracted disc folder first.

## What's in here

- `tools/ps2recomp/`: the recompiler (ELF in, C out)
- `tools/vurecomp/`: the VU1 microprogram recompiler
- `tools/`: test and check scripts
- `runtime/`: everything that stands in for the console, plus the settings menu and the mod layer
- `runtime/src/rn/`: the native renderer
- `runtime/shaders/`: the GLSL shaders, compiled at build time
- `config/`: the IDA export, symbol tables, overrides and hooks, for the US release
- `config/cnjp/`: the same files translated to the Japanese / Chinese executable
- `tools/cnjp/`: the tool that does that translation
- `tools/build_all.ps1`: one command that builds any or all three targets
- `tools/export_release.ps1`: packages a build into a folder that runs on its own
- `tools/launch.ps1`: the launcher that goes inside such a folder
- `tools/fork_and_commit.ps1`: commits this working copy to your own fork
- `runtime/dll/`: the SDL3 and pthread DLLs the build copies next to `ac5.exe`, per architecture
- `cmake/toolchain-arm64.cmake`: the Windows-on-ARM cross build
- `third_party/imgui/`: Dear ImGui, used for the settings menu
- `third_party/lua/`: Lua 5.4.9, used for mod scripts
- `tools/modkit/`: reads `DATA.PAC` by file name
- `mods/`: where mods go, see `mods/README.md`
- `tests/mods/`: the mods the mod layer is tested with
- `generated/`: the recompiler's output, which you create in step 3

## Troubleshooting

- **CMake says `Cannot find source file: .../generated/ps2_func_table.c`.** You haven't done step 3 yet, or the output went somewhere other than `generated`.
- **CMake can't find Vulkan, `sdl3` or `glslc`.** Either the PATH line from step 2 didn't run in that window, the MSYS2 packages or the Vulkan SDK aren't installed, or the terminal was already open when you installed the SDK.
- **CMake warns that the compiler has no musttail attribute.** Your GCC is too old, you need 15 or newer.
- **ac5.exe won't start and complains about a missing DLL.** See the DLL note in step 5.
- **The log says `vk: cannot open shader`.** The `shaders` folder isn't next to `ac5.exe` anymore. Put it back, or set `PS2_SHADER_DIR` to wherever the `.spv` files are.
- **The game quits by itself with `==== WATCHDOG: the guest delivered no field for 10 seconds`.** You left out `--watchdog 0`.
- **The window opens and stays black, but the log looks healthy** (disc loaded, hooks attached, 59.9 fields/s, no errors). Almost always `--data` pointing at a folder that has `ps2_image.bin` from the *other* region: the config and the executable must be the same release. `config/` goes with `generated/` and the US ISO; `config/cnjp/` goes with `generated-cnjp/` and `SLPS_254.18`. Mixing them produces a build that boots and then never leaves its first screen.
- **The black window is accompanied by `nusndstr: the transfer counter is at ...`** and nothing after it. That message is normal on the Japanese build; it means the runtime found the moved sound counter. If the scene stops advancing while the log shows the counter stuck at 0, the counter address is wrong for your executable — see [the black screen](#the-one-that-actually-caused-the-black-screen).

## Known issues

The Simplified Chinese build has two unsolved problems. The full record — symptoms, what
has been established, what was already tried, and where to look next — is in
[**`docs/known-issues.md`**](docs/known-issues.md).

1. **The radio voice language setting has no effect.** It can be set to English and
   confirmed in the new-game screen, and the mission still plays Japanese voices. PCSX2
   running the same disc *does* play English, so this is ours rather than a limitation of
   the translation patch.
2. **Exclusive fullscreen cannot be entered on this machine.** The driver refuses every
   display-mode change from this process, down to a plain `1920x1080 @ 60 Hz`, and a mode
   change is what exclusive fullscreen is. Borderless fullscreen works, and the exclusive
   path is written and takes effect on a machine whose driver allows the change.

That file also lists the dead ends and the mistakes made while chasing these two, so they
are not repeated.

## Legal

Ace Combat is a trademark of Bandai Namco Entertainment. This project isn't affiliated with or endorsed by them in any way. No game files are included, and I won't share any, so please don't ask.

The code in this repo is released under the Apache License 2.0, see `LICENSE`. Dear ImGui is MIT licensed and keeps its own license in `third_party/imgui/LICENSE.txt`. Lua is MIT licensed too, see `third_party/lua/LICENSE.html`.
