# Known issues and investigation notes

> **Work on this is paused.** The CN/JP build is playable and the picture is correct; the
> four problems below are open and nothing is in progress on them. Everything needed to
> resume — the facts established, the instrumentation that is still wired in, how to run
> each test, and the mistakes already made — is in this file.

State at the time this was written: the CN/JP (Simplified Chinese) build runs, the
picture is correct, the menus and radio captions render in Chinese, and the game is
playable. Four problems remain unsolved. This file records them along with everything
that was already tried, so the next session does not repeat it.

The reference build (US, `SLUS_208.51`) is unaffected by all of this.

---

## 1. The radio voice language setting has no effect

**Symptom.** In the new-game setup screen the radio language can be set to English and
confirmed, and the setting appears to stick, but the mission still plays Japanese
voices. Subtitles are Simplified Chinese either way, which is what the translation patch
provides.

PCSX2 running the same localized disc with English selected **does** play English
voices, so the English audio is on the disc and this is a bug on our side, not a
property of the patch.

**What is established**

The game chooses between two files on the disc, and both are present and different
sizes, so the English content was not replaced by the patch:

| File | Size |
|---|---|
| `cd:\BIN\RADIOJJ.PAC` | 629.4 MB |
| `cd:\BIN\RADIOJE.PAC` | 498.4 MB |

In the Japanese executable, the selection code is:

```asm
; 0x0015A8FC   (inside function 0x00158FB0)
lui   v3, X
addu  v20, v3, t4
lw    v3, 0xC09C(v3)        ; the quality word
lui   v2, 0x00100000
and   v3, v3, v2            ; test bit 20
beq   v3, zero, 0x0015A930  ; bit clear -> builds RADIOJE.PAC
lui   v5, 0x00420000        ; bit set   -> builds RADIOJJ.PAC
addiu v5, v5, 0x73C0
jal   0x0038171C            ; the shared loader; a1 = "\BIN\...", a2 = the file

; 0x00157EF8   (inside function 0x00156D78)
lui   v2, 0x000A
addu  v2, v2, s4
lbu   v2, 0xC13D(v2)        ; the language byte
bne   v2, 1, ...            ; == 1 -> RADIOJE.PAC, otherwise RADIOJJ.PAC
```

Both functions were hooked, and both call `0x0038171C` with the chosen filename in `a2`.

**What the instrumentation showed**

With the language set to **English**:

```
qual: call 1  s4 005CCBAC  base 0066CBAC  language byte 00668CE9 = 0
              quality 00678C48 = 00000000 bit20=0 -> RADIOJE.PAC
load: call 3  a1 = 004273C0  "RADIOJJ.PAC"
load: call 4  a1 = 01FFFD20  "\BIN\RADIOJJ.PAC"
```

Both settings point at `RADIOJE`, and `RADIOJJ` is what loads. The address arithmetic
was verified against the function's own prologue (`lbu v2, -0x3EC3(v2)` with
`v2 = 0x000A0000 + s4`), so the language byte really is being read from the right place
and really does read 0.

**Therefore** one of these is true, and the difference matters:

1. The byte at `+0xC13D` is not the field the language menu writes. It may be a
   different flag that happens not to change.
2. The byte is right, and the file is chosen by something else entirely — the branch at
   `0x0015A910` reads bit 20 of `+0xC09C`, which is a *separate* storage, and nothing
   has shown what writes it.
3. `J` and `E` in the filenames do not mean Japanese and English. They might be quality
   tiers (`RADIOJJ` is 130 MB larger), in which case the language choice is working and
   the names misled the investigation.

A single comparison run settles it: select **Japanese** and compare both the byte and
the loaded filename against the English run above.

| Japanese run shows | Meaning | Where to look next |
|---|---|---|
| `language byte` becomes 1 | the field is right; the selection ignores it | why the branch at `0x0015A910` takes the `RADIOJJ` path |
| `language byte` stays 0 | this is not the language field | the menu's write sites, found by watching `0x000A0000 + s4 - 0x3EC3` |
| `load: call 3` becomes `RADIOJE` | the option does switch files | re-check which file actually holds English audio; the names are not a reliable guide |

**Note.** The language options are the three values JP/NO SUB, EN/NO SUB and
EN/SUB(EN) — Japanese with no subtitles, English with no subtitles, and English with
English subtitles. Simplified Chinese subtitles are what the patch substitutes for the
subtitle track, so they stay Chinese whichever value is chosen; that part is expected.
What is not expected is the audio staying Japanese when an English value is selected.

**How to run the comparison**

```powershell
$env:PS2_LANG_PROBE='1'
pwsh -File tools\run.ps1 -Region cnjp -Iso "<the iso>" -Log "D:\Vibe Coding\work\J.log"
Remove-Item Env:PS2_LANG_PROBE
# select Japanese in the new-game screen, play to the first radio call, then:
Select-String -Path "D:\Vibe Coding\work\J.log" -Pattern 'qual: call 1','load: call 3' |
    ForEach-Object { $_.Line }
```

---

## 2. Exclusive fullscreen cannot be entered on this machine

**Symptom.** Choosing exclusive fullscreen leaves a normal window with its border and
title bar.

**What is established**

The original defect here was real and is fixed: the return value of
`SDL_SetWindowFullscreen` was discarded, so a refusal was indistinguishable from a mode
that had not been applied yet, and `want_fullscreen` was set anyway — which also meant
the F11 toggle believed it was already fullscreen and would never retry.

With that fixed, the refusal is visible:

```
SDL_SetWindowFullscreen -> FAILED: ChangeDisplaySettingsEx() failed: DISP_CHANGE_FAILED
```

The driver is not refusing the specific mode — it lists 275 modes including the one being
requested — and it refuses **every** mode change made from this process, including a plain
`1920x1080 @ 60 Hz`, with and without `CDS_FULLSCREEN`:

```
vk: probe: ChangeDisplaySettings(1920x1080 @ 60 Hz) returned -1
vk: applying 2560x1600 @ 165 Hz, bpp 32, fields 007C0080
vk: ChangeDisplaySettings with CDS_FULLSCREEN returned -1
vk: ChangeDisplaySettings plain returned -1
```

Exclusive fullscreen is *defined* by the mode change, so it cannot be delivered here. The
hardware is an RTX 5070 Laptop GPU on driver 576.65 — a hybrid-graphics laptop, which is
the usual reason a display mode cannot be switched by an application.

**What the runtime does now**

The runtime enters fullscreen by changing the display mode itself (borderless first, then
the mode, enumerating the driver's modes rather than passing back the desktop refresh
rate — asking for a rate the mode does not have is what produces `DISP_CHANGE_BADMODE`).
On a machine whose driver accepts the mode change this yields real exclusive fullscreen.
Where it is refused, it falls back to borderless fullscreen, which works:

```
vk: the display refused the mode change; staying borderless fullscreen
vk: fullscreen ok -- mode borderless, window 2560x1600, flags FULLSCREEN
```

`PS2_DISPLAY_MODES=1` lists what the driver offers, and the mode is restored on exit and
when switching back to windowed.

**Still worth trying, outside the code**

- Point the game at the discrete GPU (NVIDIA Control Panel → Manage 3D settings →
  `ac5.exe` → High-performance NVIDIA processor) and check whether the display is driven
  by it.
- Turn off HDR and display scaling.
- Make sure no remote-desktop or screen-sharing session is active; those disable mode
  switching outright.

If the mode change starts being accepted, the log line becomes
`exclusive fullscreen via a display mode change`.

---

## 3. Options menu: the description does not match the highlighted item

**Symptom.** In `OPTIONS` the description line under the highlighted row belongs to a
different row, and which row it belongs to changes as the cursor moves.

**Where the menu actually is.** Worth stating plainly, because an earlier investigation
looked in the wrong place: the language setting lives at `OPTIONS → LANGUAGE SETTINGS`,
**not** in the new-game setup screen. That mistake is why the `PS2_LANG_PROBE` hooks
reported nothing conclusive about problem 1 — they were written against the wrong entry
point.

**The menu data, read out of the Japanese executable**

Rows are `{label pointer, id}` pairs, eight bytes each. The ids are stepped rather than
contiguous, and one pair is out of order:

```
0040CAA0  id 213  "GAME SETTINGS"
0040CAA8  id 216  "SOUND SETTINGS"
0040CAB0  id 215  "DISPLAY SETTINGS"      <- lower id than the entry above it
0040CAB8  id 217  "LANGUAGE SETTINGS"
```

The language *values* are out of order in the same way:

```
0040C5A8  id 132 (0x84)  "JP/NO SUB"
0040C5B0  id 131 (0x83)  "EN/NO SUB"      <- lower again
0040C5B8  id 133 (0x85)  "EN/SUB(EN)"
```

The three values are Japanese with no subtitles, English with no subtitles, and English
with English subtitles. Simplified Chinese subtitles are what the patch substitutes for
the subtitle track, which is why they stay Chinese whichever value is chosen.

**What is established**

- PCSX2 on the same disc shows the correct description, so this is ours.
- The description text is **not** a string table in the executable. Each label pointer is
  referenced from exactly one place (checked), and no run of ids matching the Options rows
  exists in any section. The descriptions are loaded data, so the executable alone does not
  show them.
- The bug is therefore in how the game indexes that loaded data. The next step is a
  runtime one: find the index the menu uses for the description and log it next to the
  highlight index as the row is drawn. That draw goes through the emulated GS (it is
  outside the native renderer's range), so the hook belongs in the frontend path, with
  `tools/capture.ps1` for the visual side.

## 4. Options menu: the vibration state reads "on" when it is off

**Symptom.** With VIBRATION highlighted the description says `（当前设定为开启）` while the
setting is off.

**What is established.** That text is the game's own state string read back from the saved
setting, not a translation artifact — the pair `ON` (id 145) / `OFF` (id 146) exists in the
option-value table. So an off setting is read as on, which is the same shape as the other
state bugs in this build: a value that is stored but read back from the wrong place.

This is separate from the description mismatch above and may have a different cause; they
happened to be visible on the same screen.

## Instrumentation that already exists

These were written for the two problems above and are still in the tree. They are the
starting point for the next session.

| Switch | What it does |
|---|---|
| `PS2_LANG_PROBE=1` | hooks `0x00156D78` and `0x00158FB0` (the two radio-file decisions) and `0x0038171C` (the shared loader), printing the config values and the filename actually chosen. Declines on any build whose code does not match what it expects. |
| `PS2_DISPLAY_MODES=1` | prints the display modes the driver offers |
| `PS2_TRACE_DISP=1` | per-field display and render-target reporting |
| `--window-mode 0\|1\|2` | forces windowed, borderless or exclusive, overriding the saved setting |
| `tools/capture.ps1` | screenshots the game window through `PrintWindow`, so nothing in front of it lands in the picture |

`runtime/src/rn/rn_lang.c` is entirely diagnostic and can be deleted once problem 1 is
resolved.

---

## Mistakes made along the way, so they are not repeated

These cost several rounds. They are recorded because each one produced a plausible but
wrong conclusion.

- **The probe read an absolute `0x000AC09C`.** The addressing is
  `0x000A0000 + s4 + offset`; without `s4` the read lands somewhere the game never looks,
  and it returned zeros in both language settings, which looked like "the setting is not
  written".
- **Then it used the wrong offset `+0xC09C` for the language byte.** The language byte is
  at `-0x3EC3` from the same base; `+0xC09C` is a different field. Both live in the same
  block, which is what made the error believable.
- **Then it read at start-up instead of at the decision.** The game writes the config
  before choosing a track, so an early read only ever sees the initial state.
- **A PowerShell probe reported `ChangeDisplaySettings` as succeeding.** Its `DEVMODE`
  struct layout was wrong, so it was setting fields at the wrong offsets; the real call
  from C returns `-1`. The C-side result is the trustworthy one.
- **"Sky is fine, so the renderer is fine" was wrong twice over.** The sky dome's segment
  table and the VU1 program addresses were both wrong, and the VU1 one was invisible
  because the failure mode is "no program matches", which logs nothing.

The pattern in all of these: a wrong address or a wrong offset still returns a plausible
number, so the result looks like a finding. Verify an address against the code that uses
it (the function's own prologue) before drawing a conclusion from what is read there.
