# AGENT.md — maintaining the Plants vs. Zombies libretro core

This file is for whoever (human or agent) picks this repository up next. It
records how the port is put together, which parts are ours vs. upstream, the
non-obvious invariants that cost real debugging time, and how to verify a change.

**Paths are written as `/path/to/...` on purpose** — see
[Conventions](#conventions).

---

## 1. What this repository is

`PvZ-Portable` (upstream: <https://github.com/wszqkzqk/PvZ-Portable>) is a
cross-platform reimplementation of *Plants vs. Zombies: GOTY Edition* built on a
modernised PopCap SexyAppFramework, rendering with SDL2 + OpenGL ES 2.0.

This repository turns it into a **libretro core**: a shared library exporting the
`retro_*` API that RetroArch (or any frontend) can load instead of an executable.

The core ships **no game assets**. The player supplies `main.pak` and
`properties/` from a legally purchased copy.

### Layout

Paths here and in the scripts are relative to the **repository root** — the
checkout of this branch, i.e. the directory holding `CMakeLists.txt` and `src/`.
The libretro port is maintained as a branch on top of `main`.

```
<repo>/
├── src/                          upstream engine + our libretro port (see §3)
├── tools/libretro/               build, test and inspection scripts
│   ├── README.md                   user-facing documentation (Chinese)
│   ├── AGENT.md                    this file
│   ├── build_libretro.bat          configure + build the core
│   ├── build_libopenmpt.bat        build the MO3 music backend
│   ├── _find_toolchain.bat         toolchain detection used by the two above
│   ├── gen_libopenmpt_sources.ps1  source list for the libopenmpt build
│   ├── smoke/run.bat, smoke.c      core smoke test
│   └── paklist.py, pak_manifest.txt  main.pak inspector + asset listing
├── dist/                         staged, ready-to-use output (gitignored)
│   ├── cores/pvz_libretro.dll      the core
│   ├── info/pvz_libretro.info      RetroArch core metadata / BIOS status
│   └── system/pvz/                 game data: main.pak + properties/ (not committed)
├── third_party/                  local dependency builds (gitignored)
│   ├── libopenmpt-src/             libopenmpt source package (extracted)
│   ├── libopenmpt-cmake/           generated build for it
│   └── libopenmpt/                 its install prefix (include/ + lib/)
└── build-libretro/, build-libopenmpt/   scratch trees (gitignored, disposable)
```

`dist/system/pvz/` holds the player's own `main.pak` and `properties/`. That is
the one thing that can never be committed, and the smoke test reads it from
there — set `PVZ_SYSTEM_DIR` to test against a system directory elsewhere. A
machine that already keeps a staged tree outside the checkout can avoid a second
copy of the 43 MB pak by making `<repo>/dist` a directory junction to it.

---

## 2. Conventions

* **Never hardcode a developer's absolute path** in documentation, comments or
  scripts. Write `/path/to/repo`, `/path/to/msys64/mingw64`, etc.
  Scripts must *detect* their toolchain, or accept it from an environment
  variable — see `tools/_find_toolchain.bat` for the pattern.
* **All libretro-specific changes to upstream code live behind `#ifdef
  __LIBRETRO__`**, with a comment explaining *why* the branch exists. This keeps
  upstream merges mechanically simple: everything in an `#ifdef __LIBRETRO__`
  block is ours.
* New platform code goes in `src/SexyAppFramework/platform/libretro/`
  — never scattered through shared files. Shared files get the smallest
  possible guarded hook.
* Prefer fixing the *cause* over papering over a symptom, and say so in a
  comment. Several "obvious" workarounds in this port turned out to hide real
  bugs (see §6).

---

## 3. Architecture

### 3.1 The seam

Upstream already has a clean per-platform seam, which is what makes this port
tractable. A platform backend provides:

| Symbol | Default backend | libretro backend |
| :-- | :-- | :-- |
| `SexyAppBase::MakeWindow()` | `platform/default/Window.cpp` | `platform/libretro/Window.cpp` |
| `SexyAppBase::InitInput()` | `platform/default/Input.cpp` | `platform/libretro/Input.cpp` |
| `SexyAppBase::ProcessDeferredMessages()` | ditto | ditto |
| `SexyAppBase::StartTextInput/StopTextInput/SetTextInputRect()` | ditto | ditto |
| `SexyAppBase::DoMainLoop()` | `SexyAppBase.cpp` | guarded no-op |
| `PlatformGLInit()` | `graphics/GLPlatform.h` | routes to the frontend's `get_proc_address` |
| `GLInterface::Flush()` / `UpdateViewport()` | `graphics/GLInterface.cpp` | guarded |

Nothing else in upstream needs to know libretro exists.

### 3.2 Files we add

```
src/SexyAppFramework/platform/libretro/
├── libretro.h              vendored from libretro-common (do not edit)
├── LibretroBackend.h       shared backend interface
├── LibretroBackend.cpp     frontend callbacks, GL rendering, audio bridge, input, game clock
├── Options.h/.cpp          core options (kOptions table + GET_VARIABLE plumbing)
├── SaveState.h/.cpp        save states built on the game's mid-level snapshot
├── Window.cpp              MakeWindow(): there is no window
├── Input.cpp               InitInput / ProcessDeferredMessages / text input
└── Core.cpp                retro_* entry points + BIOS/content resolution
```

### 3.3 Three design decisions worth understanding

**Video — hardware GL, not software.**
The engine's renderer *is* OpenGL ES 2.0 and draws straight into the default
framebuffer, so the core asks for a GL context via
`RETRO_ENVIRONMENT_SET_HW_RENDER` (GLES2 first, desktop GL 2.1 as fallback) and
renders into the frontend's FBO.

A software-framebuffer port was evaluated and rejected:
`MemoryImage` never implements `BltMirror`/`StretchBltMirror`, and
`FastStretchBlt`'s tinted branch is empty, so every mirrored zombie would be
invisible and tinted stretches would vanish. Don't revisit this without a plan
for those.

**Audio — no SDL audio device.**
SDL-Mixer-X exposes a "bring your own output" API. `Mix_InitMixer(&spec, ...)`
initialises the mixer with an explicit `SDL_AudioSpec`, and
`Mix_GetGeneralMixer()` returns the mixing function, which the core calls once
per frame (44100/60 = 735 frames) and forwards to `audio_batch_cb`. No audio
thread, no device, fully synchronous.

**Frame pacing — the frontend drives.**
The game is a fixed 100 Hz logic simulation paced by a wall-clock accumulator.
`retro_run()` mirrors the Emscripten rAF callback: step `UpdateAppStep()` until
`UPDATESTATE_PROCESS_DONE` with no pending draw, so exactly one complete frame is
produced per `retro_run()`.

**Game clock — the simulation counts frames, never wall time.**
`SexyAppBase::UpdateFTimeAcc()` turns elapsed time into logic updates, and it is
the *only* place the game's update rate comes from (`mUpdateFTimeAcc` and
`mNonDrawCount` appear nowhere else). Under `__LIBRETRO__` it reads
`PvzLibretro::GameTimeMs()` instead of `SDL_GetTicks()`.

The backend's clock advances by exactly one 60 Hz frame per `retro_run()`
(`TickGameClock()`, called from `RunFrame()`), which is the same unit the audio
bridge produces: one frame of samples, 735 of them, 16.667 ms. That equality is the
whole design. Anything that counts wall time instead drifts from the music the
moment the frontend stops calling us at a steady 60 Hz:

| frontend mode | calls us | wall-clock simulation | frame-counted simulation |
| :-- | :-- | :-- | :-- |
| normal | 60 Hz | real time | real time (identical) |
| fast-forward | as fast as it can | world stayed real-time while the music ran away | both scale together |
| frame advance | once per key press | music gained 16.7 ms per press | both step one frame |
| slow motion | below 60 Hz | world stayed real-time while the music slowed | both slow together |

Two traps, both already paid for:

* **The first frame must adopt the wall clock's epoch.** The game seeds
  `mLastTimeCheck` from `SDL_GetTicks()` while it initialises; a clock starting at
  0 would make the first delta negative, and the accumulator clamps only on the
  high side, so the game would sit frozen until the two met.
* **The frame length is a fraction, not 16 ms.** `1000 / 60` truncates and the
  clock then runs 4% behind the audio. The remainder is carried in
  `sFrameFraction`.

`RETRO_ENVIRONMENT_GET_FASTFORWARDING` is no longer consulted: with the clock
counting frames, fast-forward needs no special case at all.

**Anything watched in step with the music has to use that clock.**
`SDL_GetTicks()` and `SDL_GetPerformanceCounter()` are wall time, and wall time is
now correct only for diagnostics. The credits movie ("Zombies on Your Lawn") is
pinned to its song by design — `CreditScreen` compares the elapsed time against the
animation's own length and calls `UpdateMovie()` to catch up — and it used a
`PerfTimer`, i.e. the wall clock, which left the picture behind the sound as soon
as the frontend was not running at 60 Hz. Fixed by
`PerfTimer::UseGameClock(true)` on `mTimerSinceStart`; `PerfTimer` keeps the
high-resolution counter for profiling, so the mode is per instance.

Deliberately **not** changed: `SDL_GetTicks()` elsewhere (`FindFreeChannel`'s
channel reaping, `Board::ResetFPSStats`, `LawnApp::UpdatePlayTimeStats`,
`PumpBlockingWait`'s present throttle, every `SEXY_AUTO_PERF`). Those are
bookkeeping, statistics or profiling — real time is the honest answer there.

**Save states — the game already had the snapshot.**
PvZ snapshots a level in progress on "save and exit", and again when the program
closes (`LawnApp::ShutdownHook`). That snapshot is chunked, self-describing and
complete — 20 chunks (plants, zombies, projectiles, coins, mowers, grid items,
particles, reanimations, trails, attachments, cursor, seed bank, challenge,
music) plus a ~105 field board table that includes `mBoardRandSeed` — and
`LawnSaveGame()` builds it in memory before writing it out. So a libretro save
state is that same image plus a small header for the two things the image does
not carry, both app-level: `mGameMode` and `mBoardResult`.
`LawnSaveGameToBuffer()` / `LawnLoadGameFromBuffer()` (Lawn/System/SaveGame.cpp)
are the file-free halves; the on-disk path now wraps them, so both formats stay
identical by construction.

Two rules make it safe:

* **Saving is synchronous, loading is deferred.** `retro_serialize()` only reads
  the board, which is consistent between frames even while the frontend owns the
  thread. `retro_unserialize()` instead validates the image and parks it;
  `ApplyPendingState()` runs it at the top of `RunFrame()`. The reason is the
  fiber: the frontend may call `retro_unserialize` while the game is parked
  inside a modal dialog (`Dialog::WaitForResult`), whose C++ stack still points
  into the level that is about to be replaced. `SetFiberSuspended()` marks
  exactly that window (set in `YieldToFrontend`, cleared when the fiber resumes),
  and a load waits for the dialog to finish instead of pulling the board out from
  under it.
* **A state always means "a level is playing", so loading first leaves the current
  scene.** Saving requires `SCENE_PLAYING`, but the player can press load anywhere:
  the seed chooser and the award screen are widgets that outlive the board, and
  the chooser keeps drawing *over* a level that runs underneath it (that is
  exactly how the "loaded during plant selection" bug looked). Where the scene is
  not a plain playing level, the board is torn down and rebuilt the way the game's
  own "continue" does — with `mBoardResult` parked at `NONE` first, because
  `KillBoard()` erases the on-disk save of a won or lost level. `CanRestore()`
  lists the scenes that can be left cleanly; the rest are refused, not
  half-dismantled.
* **Music is compared, not assumed.** The image records *which* tune belongs to
  the restored moment but cannot carry the decoder or its position, and the game
  only switches tunes when the wanted one differs from the one it recorded — so a
  stale stream would never correct itself. `RestoreMusic()` restarts the tune only
  when it actually changed, which keeps an in-level reload seamless and fixes the
  case where a load from the seed chooser left the intro's music playing.
* **Only a level, never the profile.** Saving is gated on `Board::NeedSaveGame()`
  — the game's own rule for a mid-level save — so menus, the Zen Garden and the
  level intro are refused. Coins, progress and the save file are never rolled
  back, so a state can neither duplicate nor destroy progress.
  The effect system has to be emptied first (see §6): a board is only ever loaded
  into a freshly freed one.

**Input — one port, three device types.**
The core declares Gamepad / Mouse / Pointer through
`RETRO_ENVIRONMENT_SET_CONTROLLER_INFO`, so the player can pick one in the
frontend's input settings. The frontend reports the choice through
`retro_set_controller_port_device()`, and `PvzLibretro::SetPortDevice()` records
it. That selection is the explicit answer to "how is this player pointing?" and
takes priority over auto-detection:

* **Mouse / Pointer selected** — the mouse drives the cursor, and the core draws
  no cursor of its own (the frontend already shows one).
* **Gamepad selected** — the pad drives a virtual cursor and the core draws it.
  Real mouse activity still takes over, because `RETRO_DEVICE_JOYPAD` is also
  what frontends report when nothing was chosen, and locking a mouse user out
  would be worse than a moment of ambiguity.
* **Nothing reported** — fall back to auto-detection: whichever device moved
  last owns the cursor.

PvZ is a point-and-click game, so the pad needs a *visible* pointer: the game
supplies no cursor artwork (under SDL it calls `SDL_CreateSystemCursor` and lets
the OS draw), and `EnforceCursor()` is a no-op here. `DrawCursor()` paints a
classic arrow for the pad-driven case only — drawing it under the OS cursor
would just look like a ghost.

### 3.4 Cooperative blocking waits (the subtle bit)

The framework has exactly one place that blocks waiting for the player:
`Dialog::WaitForResult()` spins until a button sets the dialog result. In a
libretro core that cannot work naively — the frontend only composites and swaps
*after* `retro_run()` returns, so the dialog would never be drawn, no new input
would arrive, and the host would be flagged unresponsive.

Emscripten solves this with Asyncify (`emscripten_sleep` suspends the C stack and
lets the browser's rAF loop keep running). **Fibers give the same thing
natively:**

* the game runs on its own fiber (`GameFiberEntry` → `RunFrameBody`),
* `retro_run()` just switches into it,
* `Dialog::WaitForResult()` calls `PvzLibretro::YieldToFrontend()`, which
  switches back to the frontend fiber so `retro_run()` can present and return,
* the next `retro_run()` resumes the wait exactly where it left off.

This preserves the *synchronous return value* of `WaitForResult`, which ~20 call
sites depend on — no call-site changes were needed.

Two invariants here, both of which have already caused regressions:

1. `GameFiberEntry` must call **`RunFrameBody()`**, not `RunFrame()`. `RunFrame()`
   is the switching entry point; calling it from inside the fiber makes the fiber
   switch to itself and the game silently freezes on a black screen.
2. A wait must complete a **whole** frame before yielding
   (`CompletePendingFrame()`), because one `UpdateAppStep()` is only half a frame
   — the state machine alternates between message handling and processing.
   Yielding after a single step halves the visible frame rate.

Non-Windows builds fall back to `PumpBlockingWait()`, which pumps input, present
and audio while still blocking. That keeps dialogs usable, at the cost of the
host being marked unresponsive — hence fibers are strongly preferred.

---

## 4. Building

Requirements:

| Component | Notes |
| :-- | :-- |
| MinGW-w64 gcc/g++ (C++20) | from MSYS2; must provide SDL2, zlib, libpng, libjpeg |
| CMake | the copy shipped with Visual Studio is fine |
| mingw32-make | `pacman -S mingw-w64-x86_64-make` |
| libopenmpt source package | only for background music, see §4.2 |

`tools/_find_toolchain.bat` detects MSYS2 and CMake from the usual install
locations and can be overridden:

```bat
set MSYS=C:\path\to\mingw64
set VSCMAKE=C:\path\to\cmake\bin
```

### 4.1 Core

```bat
tools\libretro\build_libretro.bat Release              :: no background music
tools\libretro\build_libretro.bat Release --openmpt    :: with background music
tools\libretro\build_libretro.bat Release --clean --openmpt
```

Output: `build-libretro\pvz_libretro.dll`. Copy it **together with**
`dist\cores\pvz_libretro.info` into the frontend's `cores/` directory.

The equivalent raw CMake invocation:

```bat
cmake -G "MinGW Makefiles" -S PvZ-Portable -B build-libretro ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=/path/to/msys64/mingw64/bin/gcc.exe ^
      -DCMAKE_CXX_COMPILER=/path/to/msys64/mingw64/bin/g++.exe ^
      -DCMAKE_MAKE_PROGRAM=/path/to/msys64/mingw64/bin/mingw32-make.exe ^
      -DCMAKE_PREFIX_PATH=/path/to/msys64/mingw64 ^
      -DBUILD_STATIC=ON -DLIBRETRO=ON -DLIBRETRO_OPENMPT=ON ^
      -DLIBRETRO_OPENMPT_ROOT=/path/to/repo/third_party/libopenmpt
cmake --build build-libretro --parallel
```

Useful options: `LIBRETRO`, `LIBRETRO_OPENMPT`, `LIBRETRO_OPENMPT_ROOT`,
`LIBRETRO_OUTPUT_NAME`, `BUILD_STATIC` (statically link SDL2/zlib/png/jpeg and
libstdc++ so the core only depends on system DLLs), `PVZ_DEBUG`.

> **Ninja note.** The `Ninja` generator works on normal machines but was observed
> to deadlock in a restricted sandbox, so the scripts use `MinGW Makefiles`.

### 4.2 Background music (libopenmpt)

The game's music is `sounds/mainmusic.mo3` — a 30-channel tracker with
Ogg-compressed samples, mixed as separate drum/hihat stems. Only libopenmpt both
registers MO3 and implements the stem API (`Mix_ModMusicStreamSetChannelVolume`,
`GetOrder`) that the adaptive music needs. **libmodplug cannot load MO3 at all.**

libopenmpt ships an autotools build (needs a POSIX shell) and MSVC binaries
(do not link into a MinGW core), so we compile its sources directly:

```bat
tools\libretro\build_libopenmpt.bat            :: extract + generate source list + build + install
tools\libretro\build_libretro.bat Release --openmpt
```

Key points:

* Needs `libopenmpt-<version>+release.autotools.tar.gz` in the repository root or
  `third_party/`; the script extracts it.
* Requires C++20. The ~150-unit source list is generated from `Makefile.am` by
  `tools/libretro/gen_libopenmpt_sources.ps1` — regenerate it after upgrading libopenmpt.
* `MPT_WITH_ZLIB` (MO3 container) and `MPT_WITH_VORBIS` + `MPT_WITH_VORBISFILE`
  (**the samples are Ogg Vorbis**) are both mandatory. Without the Vorbis pair the
  module still opens, reports the right duration and channel count, and renders
  pure silence, with only a one-line warning. Always verify by decoding to PCM
  and checking the peak — see §5.
* `libopenmpt-small` is *not* a format-reduced build; its source list is
  identical to the full library.

### 4.3 Size

The core is deliberately trimmed. Measured, with background music:

| Configuration | Size |
| :-- | ---: |
| naive (libmodplug + libopenmpt, unstripped) | 17.0 MB |
| minus libmodplug, `--gc-sections`, `-s` | 13.0 MB |
| plus `-Os` for libopenmpt | **11.5 MB** |

Levers, all enabled in the build:

* `USE_MODPLUG=OFF` — the game's data has only `.ogg` effects and `.mo3` music;
  libmodplug can load neither the MO3 nor anything else present, and libopenmpt
  supersedes it entirely.
* Only WAV / OGG (bundled stb_vorbis) / MP3 (bundled dr_mp3) / MO3 (libopenmpt)
  are enabled; FLAC, WavPack, GME, mpg123, Opus, XMP and MIDI are off.
* `-ffunction-sections -fdata-sections -Wl,--gc-sections`. **libopenmpt's static
  library must be built with the same flags**, otherwise gc-sections cannot act
  on it.
* `-s` in Release — drops the COFF symbol table; `retro_*` exports live in
  `.edata` and survive.
* `-Os` for libopenmpt only (it is the bulk of the code; decoding uses ~1.7% of
  the real-time budget).

---

## 5. Testing

```bat
tools\libretro\smoke\run.bat
```

Builds a small libretro driver (`tools/libretro/smoke/smoke.c`) that loads the core and
exercises it **without a GL context**, then checks four cases:

| Case | Expected |
| :-- | :-- |
| no content, `<system>/pvz` populated | loads (BIOS mode) |
| content = a `main.pak`, empty system dir | loads (content mode) |
| no content, empty system dir | fails with a clear message |
| content is not a `main.pak` | fails with a clear message |

It also **reproduces the frontend's array walk** over
`retro_input_descriptor` and `retro_controller_info`, asserting both are
NULL-terminated. That check exists because an unterminated
`retro_controller_info` crashed RetroArch during load — keep it.

When adding a behaviour, prefer extending this test over adding temporary
logging to the core. Debug logging that is added "just to diagnose" has a habit
of shipping; the core should be silent during normal play.

For audio changes, verify decoding out-of-band rather than by ear: extract
`sounds/mainmusic.mo3` with `tools/libretro/paklist.py`, decode it with libopenmpt, and
assert the rendered peak is non-zero. A silent-but-successful load is the
characteristic failure mode here.

`tools/libretro/paklist.py` is the general-purpose `main.pak` inspector:

```sh
python tools/libretro/paklist.py /path/to/main.pak list
python tools/libretro/paklist.py /path/to/main.pak extract /path/to/outdir
python tools/libretro/paklist.py /path/to/main.pak cat sounds/mainmusic.mo3 > out.mo3
```

(Format: whole file XOR 0xF7, header included; records carry name/size/filetime,
payload follows the directory.)

---

## 6. Hard-won invariants

Read this before debugging. Every item below was an actual failure.

### libretro API contract

* **A core must export all 25 `retro_*` symbols**, including the ones that do
  nothing (`retro_cheat_reset`, `retro_cheat_set`). RetroArch resolves every
  symbol up front and refuses to load a core missing any of them. "Failed to load
  symbol" in the log means exactly this.
* **`retro_controller_info` and `retro_input_descriptor` arrays must be
  NULL-terminated.** Frontends walk them until the terminator and will read past
  the end otherwise.
* **`RETRO_DEVICE_ID_POINTER_COUNT` is not implemented by every frontend.**
  RetroArch's dinput driver returns 0 for it. Do **not** use it to decide whether
  a pointer is available — that silently pushes input onto the relative-mouse
  path, and clicks then land somewhere other than where the cursor is drawn.
* **Some input ids are one-shot pulses.** `RETRO_DEVICE_ID_MOUSE_WHEELUP` /
  `WHEELDOWN` are consumed by the read; reading an id twice per frame loses the
  event. Read each id exactly once per frame.
* **`RETRO_HW_CONTEXT_OPENGLES2` needs a frontend built with `HAVE_OPENGLES`.**
  Windows RetroArch builds usually are not, so the GL 2.1 fallback is the normal
  path — not an error.
* Core contexts (`glcore`) reject GLSL 1.20, `attribute`/`varying`/
  `gl_FragColor`/`texture2D`. `shaderCompile()` retries as GLSL 1.50 core, which
  is why both `gl` and `glcore` work.
* **The frontend's environment callback is not thread safe.** Only call it from
  the thread the frontend drives; the loading thread reports errors through
  `Popup()`, which must not forward to the frontend.
* **A frontend may swallow keyboard input that is bound to its own controls.**
  RetroArch drops any key that is bound to a RetroPad button or to a hotkey
  before it reaches the core's keyboard callback, unless Game Focus Mode is on —
  and `input_auto_game_focus` defaults to `off`, so registering
  `RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK` alone is not enough. Keys bound by
  default include `A/Q/W/E/R/S/X/Z`, the arrows and Enter, i.e. text entry would
  silently lose characters. The raw `RETRO_DEVICE_KEYBOARD` state is *not*
  filtered, which is why `PollKeyboardKeys()` in `LibretroBackend.cpp` polls it
  every frame and synthesizes the presses (and characters) the callback did not
  deliver. Keep both paths: the callback carries the frontend's own character
  translation, the poll carries what the callback never saw. `sKeyDown[]` is the
  shared edge detector and `sKeySynthesized[]` stops a late callback event from
  delivering the same press twice — break either and typing either doubles or
  dies. The same poll is what makes an overlay/on-screen keyboard usable: those
  only report key states, never characters.
* **Text entry is a hard gate, not a nicety.** The first launch blocks on the
  profile-name dialog and an empty name is rejected with no way out, so a
  frontend that cannot deliver typed text would soft-lock the game. When
  `RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES` answers without
  `RETRO_DEVICE_KEYBOARD` (an unanswered query means "assume a keyboard"),
  `LawnApp::DoCreateUserDialog()` pre-fills `Player` via `SetName()`, which also
  selects it, so typing still replaces it.
* **`retro_serialize_size()` is a promise about the buffer, not about the
  payload.** RetroArch stores a block of exactly that size and hands exactly that
  size back to `retro_unserialize` (`tasks/task_save.c` passes the block length,
  with a comment saying cores depend on it). A load must therefore accept
  `size == announced` no matter how small the image inside is — comparing the two
  for equality silently rejects every real state, which is exactly the bug this
  had. The image length lives in the core's own header and only has to *fit*.
  The same block also has to be zeroed on save, or the unused tail of the
  frontend's buffer ends up in the state file.
* **Nothing of ours may still be running when the frontend unloads the DLL.**
  A frontend `FreeLibrary`s the core right after `retro_unload_game()`, so every
  thread the core started must be joined before that returns — otherwise it wakes
  up executing unmapped memory and takes the whole frontend down. The report is
  unmistakable: the module is named **`pvz_libretro.dll_unloaded`** and the fault
  is inside SDL (`SDL_SemWait_atom`). The culprit was the **SDL timer thread**:
  `SexyAppBase`'s constructor calls `SDL_Init(SDL_INIT_TIMER)`, `SDL_TimerInit()`
  creates a thread that parks on a semaphore, and nothing ever called
  `SDL_Quit()`/`SDL_QuitSubSystem(SDL_INIT_TIMER)`. One thread leaked per content
  load, which is why the dump showed several threads on the same stack and `lm`
  listed four different `pvz_libretro.dll` base addresses. Both teardown paths
  (`retro_unload_game`, `retro_deinit`) now end with `SDL_Quit()`, which runs
  `SDL_TimerQuit()` — it posts the semaphore and *joins* the thread, so there is no
  race. Adding a thread to this core means adding its shutdown here.
* **Diagnosing a core crash from a frontend dump.** Windows keeps them in
  `%LOCALAPPDATA%\CrashDumps`. `cdb.exe` (Windows Kits debuggers) reads one with no
  symbols: `cdb -z <dump> -cf <command file>` where the file holds `.ecxr`, `k` and
  `~*k`. Frames inside the core come out as `pvz_libretro+0x<rva>` or
  `<Unloaded_pvz_libretro.dll>+0x<rva>`, and that RVA maps to a function by
  relinking the objects without `-s` (see §8) and asking `nm` for the nearest
  symbol; a source line comes from the `.loc` directives in `g++ -S -g` output for
  that one translation unit. Unwinding stops at an unloaded image, so the *shape*
  of the stacks — how many threads, and whether they are identical — is often the
  real evidence.
* **A state may only be restored into an *empty* effect system.**
  `Board::Board()` and `Board::DisposeBoard()` both open with
  `EffectSystemFreeAll()`, and that is a precondition of the save format, not a
  nicety: the image rebuilds every particle/trail/reanimation/attachment list
  through the two fixed node allocators, so a restore into the live effect system
  of the level being replaced re-allocates nodes the old level still holds. The
  pool runs dry, the lists end up pointing at nothing, and a later allocation
  hands out a slot with a garbage `mEmitterDef` — symptom: an access violation in
  `PvzpParticleEmitter::Update()` (`mEmitterDef->mParticleFlags`), a second after
  the load, and only in levels busy enough to exhaust the pool. `ApplyPendingState()`
  therefore calls `EffectSystemFreeAll()` before the restore. Debugging note: the
  crash offset from the Windows event log maps to a function through an
  unstripped relink (`link.txt` minus `-s`), then to a line through `.loc`
  directives in `g++ -S -g` output for that one translation unit.
* **A save state may only be applied between frames.** `retro_unserialize` is
  called by the frontend on its own terms, including while the game fiber is
  parked inside a modal dialog. Never restore the board from inside
  `retro_unserialize`: park the image and let `ApplyPendingState()` (top of
  `RunFrame()`) do it. `YieldToFrontend()` marks the unsafe window through
  `SetFiberSuspended()`.
* **Core options are registered from one table and re-read every frame.**
  `Options.cpp` holds `kOptions`; `RegisterOptions()` (called from
  `retro_set_environment`) publishes it through `SET_CORE_OPTIONS_V2` and falls
  back to the legacy `SET_VARIABLES` string form when the frontend refuses.
  Two contracts bite silently: the definition array must end with a zeroed entry,
  and every `default_value` must equal one of that option's own values or the
  frontend drops the option without a word — the smoke test checks both.
  `RefreshOptions()` runs once per frame (gated on `GET_VARIABLE_UPDATE`) and
  applies the runtime-readable ones in `ApplyOptions()`; never cache a value at
  startup, or toggling it in the menu will not do anything.
* **Upstream's switches are build-time or command line, and the core has
  neither.** `Core.cpp` calls `gLawnApp->SetArgs(0, nullptr)`, so *no* `-` flag
  reaches the game; `PVZ_DEBUG`, `DO_FIX_BUGS` and `LOW_MEMORY` are `#ifdef`s.
  Before adding a core option, check whether upstream already exposes the thing
  at runtime — `-cheat`, for instance, only sets `mCheatKeys` and
  `mDebugKeysEnabled`, two plain booleans, so it needs no rebuild to become a
  core option.

### SDL-Mixer-X

* **`Mix_InitMixer` requires a fully populated `SDL_AudioSpec`, including
  `size`.** `Mix_OpenAudio` used to get `size` filled in by
  `SDL_OpenAudioDevice`; `Mix_InitMixer` does not. Leaving it 0 makes
  `Mix_LoadMusic_RW` — the OGG/MP3 fallback of `Mix_LoadWAV_RW` — decode zero
  bytes per iteration while the decoder still reports "playing", appending empty
  fragments to a linked list forever. Symptom: the loading thread never finishes,
  a black screen after the logo, and memory growing at hundreds of MB/s.
* **`Mix_GetGeneralMixer()` must not be called before `Mix_InitMixer`,** and it is
  unsafe to mix while the loading thread is decoding sounds (there is no SDL
  audio lock when mixing by hand). The core gates mixing on
  `mLoadingThreadCompleted`.
* The mixer's C sources include libopenmpt's headers, which use C99+ syntax, so
  `CMAKE_C_STANDARD` is raised to 11.

### Build system

* **Do not put `CMake/` on `CMAKE_MODULE_PATH`.** It contains an
  Emscripten-only `FindSDL2.cmake` that `FATAL_ERROR`s elsewhere and would shadow
  SDL2's own config package.
* **SDL-Mixer-X's `cmake/find/FindOpenMPT.cmake` uses bare `find_path`/
  `find_library` with no hints.** Preset its `OpenMPT_INCLUDE_DIR` and
  `OpenMPT_LIBRARY` cache variables, or `find_package(OpenMPT)` reports not-found
  and the mixer silently skips its MO3 backend (`-- skipping libopenmpt --`).
* A static archive does not carry its dependencies: `libopenmpt.a` needs
  libvorbisfile/libvorbis/libogg/zlib at the final link.

### Upstream quirks

* **`Sexy::LogInfoLn` does not reach the frontend log.** It goes to
  `SDL_LogMessage`. `DispatchLogLn` mirrors it into the libretro log callback
  *and must add the trailing newline itself* — `LogInfoLn` implies a newline that
  only the file sink writes, so without it every game log line runs together.
* `openmpt_get_library_version()` / `openmpt_get_core_version()` return a packed
  **`uint32_t` version number**, not a string. Use
  `openmpt_get_string("library_version")` for text. Treating the integer as a
  pointer is a fast route to a confusing access violation.
* `ResourceManager`'s group-loading state (`mCurResGroupList`,
  `mCurResGroupListItr`) is shared, not per-thread. Loading from two threads at
  once would corrupt it.
* `Dialog::WaitForResult` is the only blocking wait in the framework — see §3.4.

---

## 7. Upstream vs. ours

So that a future upstream merge is mechanical:

**Added (no conflicts possible)**
`src/SexyAppFramework/platform/libretro/*`, `CMake/libretro/` (if present),
`dist/`, `tools/`, `README-libretro.md`, `AGENT.md`.

**Modified (edits wrapped in `#ifdef __LIBRETRO__`; the rows marked *addition* are
new functions that change no existing behaviour and are left unguarded)**

| File | Change |
| :-- | :-- |
| `graphics/GLPlatform.h` | `PlatformGLInit()` resolves GL through the frontend |
| `graphics/GLInterface.{h,cpp}` | `GfxReapplyState()`; fixed viewport; `Flush()` does not swap or clear; core-profile shader retry |
| `SexyAppFramework.{h,cpp}`, `SexyAppBase.{h,cpp}` | `DoMainLoop()` no-op; teardown guard; `nanosleep` disabled; `DoExit` never exits; cursor calls skipped; `Popup` logs instead of showing a dialog; `LibretroTeardown()`; `UpdateFTimeAcc()` reads the backend's game clock |
| `sound/SDLSoundManager.cpp` | `Mix_InitMixer` instead of `Mix_OpenAudio`; `Mix_FreeMixer`; `Initialized()` no longer needs the SDL audio subsystem |
| `widget/Dialog.cpp` | `WaitForResult()` yields to the frontend |
| `widget/CreditScreen.cpp` | *(addition)* the credits movie clock opts into the game clock (`PerfTimer::UseGameClock`) |
| `misc/PerfTimer.{h,cpp}` | *(addition)* optional game-clock mode; the wall-clock counter stays the default for profiling |
| `Common.cpp` | `DispatchLogLn` mirrors to the frontend log with a newline |
| `CMakeLists.txt` | `LIBRETRO` / `LIBRETRO_OPENMPT` options, the core target, size flags |
| `LawnApp.cpp` | `FrontendHasKeyboard()` name seeding in `DoCreateUserDialog()` |
| `Lawn/System/SaveGame.{h,cpp}` | *(addition)* `LawnSaveGameToBuffer()` / `LawnLoadGameFromBuffer()`: the mid-level snapshot's file-free halves, extracted from `LawnSaveGame()` / `LawnLoadGameV4()` without changing either format |

When touching a shared file, add the guarded branch and a comment explaining the
reason. Do not reorder or reformat surrounding upstream code; that is what makes
merges painful.

---

## 8. Debugging playbook

The symptoms seen so far, and what they actually meant:

| Symptom | Cause |
| :-- | :-- |
| `Failed to load symbol: "retro_cheat_reset"` | core missing an API symbol (§6) |
| Log stops right after `SET_CONTROLLER_INFO` | unterminated `retro_controller_info` |
| PopCap logo then black screen, memory climbing | `SDL_AudioSpec.size` unset → infinite decode loop |
| PopCap logo then black screen, memory flat | a `[pvz] loading:`-style trace is the fastest way in; the loading thread stalls before it counts tasks |
| Dialog never appears, game frozen | blocking wait cannot present (§3.4) |
| Dialog appears but the host is "Not Responding" | wait is blocking instead of yielding |
| Dialog appears but the frame rate is halved | yielded after a single `UpdateAppStep` |
| Total black screen, no sound | game fiber called `RunFrame()` instead of `RunFrameBody()` |
| Mouse cursor and clicks disagree | gated on `RETRO_DEVICE_ID_POINTER_COUNT` |
| Gamepad player sees no cursor at all | the game draws no cursor and the core had none; see `DrawCursor()` in §3.3 |
| Wheel does nothing | wheel id read twice in one frame |
| `music failed to load` | libopenmpt missing, or the MO3 opened but decoded silence (no Vorbis) |

If you need temporary instrumentation, add it behind `__LIBRETRO__`, keep it
**one line**, and remove it before finishing. Prefer extending
`tools/libretro/smoke/smoke.c` with a real assertion instead.
