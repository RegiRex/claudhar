# Multicore 3DS Emulator Rebuild — Plan

**Status:** approved 2026-10-14; Phase 0 in progress · **Repository:** RegiRex/claudhar
**Builds on:** the multi-core architecture schematic and the Uberhar audits (shared with
the owner outside this repository).

---

## 0. Summary

We build a new Android-first 3DS emulator organized as a pipeline of threads, so the
guest CPU, GPU command processing, geometry, rasterization, audio and presentation
run at the same time instead of taking turns on one thread. Two rules come before
speed:

1. **No shader compilation pause, ever.** The renderer never waits for a shader or
   pipeline compile while a game runs. Everything the game can need is either compiled
   once before play or does not need compiling at all.
2. **No skipped draw, ever.** Every guest draw runs exactly once and in order. Counters
   prove it in every build, and tests fail the build if either rule is broken.

"Ground-up" applies to the parts that decide threading and rendering: the main loop,
timing, the GPU front end, the geometry and raster pipelines, the Vulkan backend,
presentation and the frame scheduler. Those are about 25–35 thousand new lines.
Parts that decide only *what* the 3DS does, not *when* or *on which core*, are reused
from Azahar through thin adapters:

- system services, file systems and the game loader;
- cryptography and the ARM translator (Dynarmic);
- audio decoding;
- the Android interface.

That is about 150 thousand lines of mature, game-tested code. Rewriting it would cost
years and add no multicore benefit.

Every phase ships a side-by-side test APK through GitHub Actions with a direct
download, as the Uberhar Claude builds do now. Each phase is measured against stock
Azahar on your devices before the next one starts.

---

## 1. Your requirements and how the design answers them

| Requirement | Design answer |
|---|---|
| Maximize CPU core use | Thread pipeline plus one shared job system sized from the device's actual cores (Sections 5–6). The CPU renderer scales across every core. |
| Retroid Pocket Duo is the minimum | All budgets, thread placement and Vulkan features target the Duo's chip and driver. Anything newer is a bonus, not a requirement (Section 2). |
| Efficient code, as few and light lines as possible | Rewrite only the timing and rendering core. Reuse everything else. Each new module has a line budget and a measured per-frame cost (Sections 3 and 10). |
| Comments on all written code | Every new file, function and non-obvious block carries a purpose comment, checked in review (Section 10). |
| Replicate Azahar's interface | Reuse Azahar's Android app and replace only the graphics settings page (Section 8). |
| Own graphics settings | A short new settings page (Section 8). No Uberhar debug toggles. |
| Native and Combo equivalents for testing | **Hybrid** (CPU geometry, GPU pixels) and **Combo** (Hybrid plus background-specialized shaders) modes. A third **CPU** mode runs all graphics on the CPU (Section 7). |
| No shader compilation pauses or draw skipping (premier focus) | Section 4. Enforced by counters and failing tests. |
| Citra MMJ as a reference | Used for ideas only, from its published source (Section 12). |
| Thor, Duo and DDS dual screens and controllers work automatically | Reuse Azahar's display and controller handling and add a known-device table (Section 9). |

---

## 2. Target hardware

| | **Retroid Pocket Duo (minimum)** | AYN Thor (Base/Pro/Max) | Anbernic RG DDS |
|---|---|---|---|
| SoC | Qualcomm Dragonwing Q-7790 (Snapdragon 7 Gen 4 class) | Snapdragon 8 Gen 2 | Snapdragon G2 Gen 2 |
| CPU | 1× Cortex-A720 2.6 GHz, 4× A720 2.4 GHz, 3× A520 1.8 GHz | 1× X3, 2× A715, 2× A710, 3× A510 | octa-core |
| GPU | Adreno 722 | Adreno 740 | Adreno (G2 Gen 2) |
| RAM | 8 GB (LPDDR4X per most listings) | 8/12/16 GB LPDDR5X | 8 or 12 GB (unconfirmed) |
| Screens | 5.5" 1080p top + 4.5" 960p bottom, 120 Hz | 6" 1080p 120 Hz top + 3.92" 1240×1080 60 Hz bottom | 4.7" 1200×720 top + ~4" 960×720 bottom touch |

What the baseline device forces:

- **No native 32-bit ARM execution.** The A720 and A520 cores cannot run 32-bit ARM
  code, so guest code always goes through the ARM translator (Dynarmic). The native-
  execution idea in the schematic is dropped.
- **Only five fast cores.** The Duo has one prime core and four big cores. The guest's
  main CPU stream gets the prime core. The four big cores carry GPU emulation and
  geometry. The three small cores take light, latency-tolerant work.
- **The GPU uses Qualcomm's own driver.** Upstream Mesa Turnip does not support the
  Adreno 722; only experimental community builds exist. The design cannot assume
  optional Vulkan features and must measure what the Duo's driver offers before
  choosing (Phase 0).
- **Frame budget.** 16.7 ms per guest frame at 100% speed. The guest CPU stream
  cannot be split, so the emulator's own frame time can approach that stream's cost
  but not go below it. Everything else must run alongside it.

---

## 3. What is rewritten and what is reused

| Area | Azahar size | Plan | Why |
|---|---:|---|---|
| Main loop, frame pacing, timing | (in core, ~5k) | **Rewrite** | This is where Azahar serializes everything on one thread. |
| GPU front end (PICA command processing) | (in video_core) | **Rewrite** as its own thread | Moves GPU emulation off the guest CPU thread; the biggest multicore win. |
| Geometry (vertex shading, assembly, clipping) | (in video_core) | **Rewrite** around the job system; reuse Azahar's ARM64 shader JIT to start | Spreads vertex work across cores without per-draw fork-joins. |
| Vulkan renderer | ~25k (est.) | **Rewrite**, smaller | Built for zero runtime compiles (Section 4). |
| CPU renderer | ~3k | **Rewrite** as a tiled multicore renderer | The one renderer that uses every core. |
| OpenGL renderer, Qt desktop app | ~40k | **Drop** | Vulkan only; Android first. |
| Memory system | ~3k | **Rewrite** core (fast host mapping, page protection) | Lets threads share guest memory safely without per-access checks. |
| Kernel (threads, sync, IPC) | 12k | **Reuse first**, made thread-safe in Phase 7 | Only matters once guest cores get their own threads. |
| System services (HLE) | 66k | **Reuse** | Behavior, not threading; years of compatibility fixes. |
| File systems, loader, crypto | 15k + | **Reuse** | Same. |
| ARM translator (Dynarmic) | external | **Reuse** | Best available; the Duo cannot run 32-bit ARM natively. |
| Audio (DSP HLE) | 6k | **Reuse**, moved to its own thread | Removes audio from the guest CPU thread. |
| Android interface | 28k Kotlin | **Reuse**, rebranded, new graphics page | You asked for Azahar's interface. |
| Input, network, multiplayer | 9k | **Reuse** | Behavior only. |

New code target: about 25–35k lines, plus about 2k lines of adapters.

---

## 4. Zero compile pauses and zero skipped draws (premier focus)

### 4.1 The rules, enforced

| Rule | How it is enforced |
|---|---|
| R1. Every draw executes once, in order | A draw ledger counts requested, drawn, empty and invalid draws. `requested = drawn + empty + invalid`, and `skipped = 0`. Tests replay recorded GPU traces and fail on any mismatch. |
| R2. No draw waits for a shader or pipeline compile | Every wait on a compile is counted (`compile_waits`). It must be 0 in every test and every device log. A wait during play is a bug, not a slow path. |
| R3. No frame is held back for optional work | Specialization, warm-up and statistics run on background threads and are never waited on. |
| R4. Presentation may drop or repeat *displayed* frames for pacing; the guest's rendering never skips | Kept separate in counters and logs. |

### 4.2 How the GPU modes avoid compiling during play

The 3DS fixed pipeline has a huge number of fragment configurations. Azahar
compiles a specialized shader for each new one, which is the source of cold-run
pauses (or skipped draws when asynchronous compilation is on). The rebuild removes
that need:

1. **CPU geometry, one vertex format.** Vertices are shaded on the CPU, so the GPU sees
   one fixed vertex layout and one trivial vertex shader.
2. **One color format and one depth format on the host.** All 3DS color formats render
   to RGBA8, and all depth formats to one depth-stencil format. Conversions happen
   only when guest memory reads or writes those surfaces, as CPU jobs.
3. **A fragment ubershader.** One shader evaluates every 3DS fragment feature from data:
   texture combiner stages, lighting with lookup tables, fog and gas, procedural
   textures, shadows, alpha test and texture formats (decoded on the CPU). The
   configuration lives in uniform buffers, not in shader code.
4. **Dynamic state for the rest.** Vulkan 1.3 core dynamic state covers depth, stencil,
   cull mode, front face, topology, viewport and scissor.
5. **Blending, logic ops and write masks** are the part Vulkan 1.3 cannot make dynamic.
   Phase 0 measures which of three methods works on the Duo, and the renderer picks
   one per device:

   | Method | Pipelines needed | Requirement |
   |---|---|---|
   | A. Dynamic blend state | a handful | `VK_EXT_extended_dynamic_state3` blend/logic/write-mask features |
   | B. Finite blend-variant set, all compiled before play | tens to low hundreds, created from one cached shader | Measured fast creation for blend-only variants from a warm pipeline cache |
   | C. Blending in the shader | a handful | Raster-ordered framebuffer read (Vulkan extension) or an OpenGL ES path with framebuffer fetch |

   B works on any Vulkan driver because the full set is created before play. A and C
   reduce the set.

6. **Compile everything before play, once.** On first launch, and after an app or
   driver update, a one-time "Preparing graphics for this device" screen builds the
   whole pipeline set on all cores and stores it in the driver's pipeline cache. Phase
   0 measures how long this takes on the Duo; the goal is under 30 seconds once per
   device. No pipeline is ever created on demand during play.

### 4.3 The modes and compilation

| Mode | What can be compiled | When | During play |
|---|---|---|---|
| Hybrid | Fixed pipeline set (Section 4.2) | First launch | Nothing compiles |
| Combo | Same set, plus specialized fragment shaders and optional GPU vertex shaders | Background threads only | The ubershader keeps drawing until a specialized pipeline is finished. GPU vertex shading is used only for programs whose pipeline is already built; otherwise the CPU shades. Never waited on. |
| CPU | Nothing (an interpreted fragment path is always ready) | A faster compiled path builds in the background | Swapped in when ready, never waited on |

### 4.4 Other sources of hidden skipping

- **Custom (high-resolution) textures** load on I/O threads. The original texture is
  used until the replacement is ready, or everything loads at boot (the "preload"
  option).
- **"Skip duplicate frames" style shortcuts** are not ported.
- **Draws with no valid render target or invalid input** are counted and logged, never
  silently dropped, and leave no state behind for the next draw. This fixes the
  behavior found in the Uberhar audit.

---

## 5. Multicore execution

### 5.1 Thread pipeline

```
Guest CPU thread (prime core)
  Dynarmic ARM11 core 0 + kernel + HLE services
    |  GSP commands (stamped with guest time)
    v
PICA thread (big core)          DSP thread (small core)      I/O thread(s) (small cores)
  parse command lists              audio HLE, one audio         ROM, saves, texture packs,
  intern GPU state                 frame behind the guest       pipeline cache
  emit draw packets
    |
    v
Job system workers (big cores): vertex shading, clipping, texture decode, CPU raster tiles
    |
    v
Vulkan recorder thread (big core) -> GPU          Presenter thread (small core) -> both screens
```

- **Guest CPU thread.** Runs the game. Nothing else runs here except kernel and service
  calls the game makes.
- **PICA thread.** Takes GPU command lists off the guest thread. The guest keeps
  running. Completion interrupts are delivered at the guest time the timing model
  computes, and never before the host has actually finished the work.
- **Guest memory safety.** Pages that in-flight GPU work reads, or that only the GPU
  has written, are protected. A guest access to one waits only for that page. This
  replaces Azahar's check on every guest memory access.
- **Accurate mode.** Processes GPU commands synchronously, exactly like Azahar, for
  any game the pipeline breaks. It is selectable per game.

### 5.2 Placement on the Retroid Pocket Duo

| Cores | Work |
|---|---|
| Prime A720 (2.6 GHz) | Guest CPU thread |
| 4× A720 (2.4 GHz) | PICA thread, Vulkan recorder, 2 job workers (all 4 workers in CPU mode) |
| 3× A520 (1.8 GHz) | Guest system core (lightly used), DSP/audio, presenter and input, background compile and I/O |

- The Android performance hint API tells the scheduler each frame's work time against
  the 16.7 ms budget, so clocks rise before frames run late.
- Hard core pinning is used only if measurements show it helps.
- There is one job system. No subsystem creates its own thread pool. That avoids the
  roughly 35 overlapping threads Uberhar reached.

### 5.3 Later: guest cores on separate threads (Phase 7)

The 3DS's second core (system) and the New 3DS's extra cores can each get a host
thread. This needs a thread-safe kernel. It helps New 3DS titles and games that use
the second core, but not the main game stream, so it comes after the GPU pipeline
work, which helps every game.

---

## 6. Rendering pipeline detail

- **Draw packets.** The PICA thread turns each draw into a packet: references to
  immutable state blocks plus vertex and index ranges. No register copies.
- **Geometry jobs.** Vertex shading runs as jobs, split across workers for large draws
  and batched for small ones. The 3DS vertex cache order is planned once, so results
  match hardware exactly. Strips and fans become plain triangle lists.
- **Vulkan recorder.** Records ordered draws, merges consecutive draws to the same
  target into one render pass, and folds clears into pass loads. On phone GPUs each
  extra pass costs memory traffic that grows with internal resolution.
- **CPU raster.** The screen is divided into 8×8 tiles that match the 3DS framebuffer
  layout. Each worker owns whole tiles and processes triangles in submission order,
  so output matches hardware ordering exactly. This generalizes the band renderer
  already measured on the Claude branch: 16 ms against 32 ms on one core, on a 4-core
  test host.
- **Textures.** Decoded once per change on workers into the one host format.
  Replacement packs load on I/O threads.

---

## 7. Test modes (Native and Combo equivalents)

| Mode | Vertex work | Pixel work | Compiles during play | Uberhar equivalent |
|---|---|---|---|---|
| **Hybrid** | CPU, multicore jobs | GPU ubershader, precompiled | None | Native |
| **Combo** | CPU; GPU only for already-built programs | GPU ubershader, swapping in background-specialized shaders | Background only, never waited on | Combo (Automatic) |
| **CPU** | CPU | CPU tiles on every core; GPU only presents | None | CPU Software |
| **Accurate** (per-game option) | Same as the selected mode, but GPU commands are processed synchronously | | | Azahar's ordering |

There is no Calculated mode. GPU compute rendering goes against the goal of keeping
work on the CPU.

---

## 8. Interface and graphics settings

- Azahar's Android app is reused as-is: game list, per-game settings, controls,
  overlays, cheats, Amiibo, multiplayer, layouts, hotkeys and the screen setup flows.
  It is rebranded, and the graphics settings page is replaced.
- New graphics page:
  - Renderer mode (Section 7)
  - Internal resolution (1x–4x for GPU modes; 1x or 2x for CPU mode)
  - Texture filter
  - Custom textures: off, load in background, or preload
  - Stereo 3D mode
  - Simulated 3DS GPU timing (on by default)
  - Frame pacing and V-Sync
  - Speed limit and turbo
  - Per-game Accurate GPU timing
  - "Prepare graphics again" button
- Removed: asynchronous shaders, the disk shader cache toggle (the cache is always
  used), SPIR-V and optimizer toggles, "skip duplicate frames", and every Uberhar
  experiment switch.

---

## 9. Dual screens, device detection and controllers

**What base Azahar actually does** (checked in its source):

- **Second screen.** It finds any second display through Android's standard display
  APIs and shows the bottom screen there, with a workaround for the Odin 2's built-in
  virtual display. It does not look up the device model.
- **Controllers.** Built-in controls appear as standard Android gamepads. Azahar has
  default mappings, an auto-map dialog that detects Nintendo vs Xbox button order,
  and fixes for a few known controllers.

**The rebuild:**

1. Reuses both mechanisms unchanged, so any dual-screen device works the way it does
   in Azahar today.
2. Adds a small known-device table keyed by Android's manufacturer and model strings,
   for the Thor, Retroid Pocket Duo and RG DDS. Each entry sets:
   - which physical display is the top and bottom 3DS screen;
   - the default screen layout and touch mapping;
   - the face-button layout (Nintendo or Xbox order);
   - the built-in controller's default mapping, applied on first launch with no
     setup step.
3. Unknown devices use Azahar's generic behavior. The app logs the device's model
   strings so new entries can be added from a log alone.
4. The exact model strings for the Duo and DDS must be read from real units (the app
   logs them). Entries are confirmed on a device before they ship.

---

## 10. Code standards

- C++20 for the core and Kotlin for the app. Vulkan only.
- **Comments.** Every new file starts with a purpose paragraph. Every function says
  what it does, which thread calls it and what it touches. Every non-obvious block
  says why. Review fails code without them.
- **Lean code.**
  - Each new module has a line budget, below. Going over it needs a reason in review.
  - One way to do each thing: no parallel experimental paths in the hot loop.
  - No per-draw allocation, no per-pixel virtual calls, no logging in hot loops.
  - Hot loops are profiled before and after each change.

| Module | Line budget |
|---|---:|
| Main loop, timing, frame scheduler | 2,500 |
| Job system and thread management | 1,000 |
| PICA front end | 3,500 |
| Geometry stage | 3,000 |
| Vulkan backend | 9,000 |
| CPU tile renderer | 6,000 |
| Presenter | 1,500 |
| Memory system (fast mapping, protection) | 2,000 |
| Adapters to reused Azahar code | 2,000 |
| Device table and app changes | 1,000 |

- **Measured, not assumed.** Each change records its frame-time effect on the test
  scenes. The Claude branch lost speed against the main branch because several
  untested ideas shipped together. Here each idea lands alone and is kept only if it
  helps on hardware.

---

## 11. Phased delivery

Every phase ends with a direct-download test APK that installs next to Uberhar and
Azahar, and a short report. Your test scenes: the Dark Moon opening (File 1 reset,
shader cache deleted) and FEA, compared with stock Azahar on the same device and
settings.

| Phase | Delivers | Done when |
|---|---|---|
| **0. Groundwork** | New repository and CI (direct APK). A device-probe screen that records Vulkan features, blend-variant creation cost, core layout, displays and controllers. Stock Azahar baselines on your devices. | Probe results from the Thor ([findings](PHASE0_FINDINGS.md)). Blending method chosen (Section 4.2), with method B as the fallback any driver can use. The Duo's probe, when it arrives, confirms or narrows the choice before Phase 2 is tested on it. |
| **1. New core on reused parts** | Rewritten main loop, timing, job system, PICA thread (Accurate mode first), DSP thread, presenter. Azahar services and loader adapted. Bring-up renderer: CPU mode, single-threaded. | Homebrew and Dark Moon boot and play at reduced speed with both screens, controls and audio. `skipped = 0`, `compile_waits = 0`. |
| **2. Hybrid mode** | Precompiled ubershader Vulkan backend, "Preparing graphics" screen, one host format, render-pass merging. | Cold first run of Dark Moon with no compile pauses (counter 0) and no skipped draws. Speed compared with Azahar. |
| **3. Multicore pipeline** | Asynchronous PICA with guest-time completion and page protection; geometry and texture jobs; performance-hint session; per-game Accurate fallback. | Measured frame-time gain over Phase 2 and over Azahar on the Thor. Thread activity shows the planned spread. |
| **4. Combo mode** | Background specialization and optional GPU vertex shading for ready programs. | Same zero counters. Combo faster than Hybrid at 2x+ on at least one test scene, or reported as not helping. |
| **5. CPU mode, multicore** | Tiled CPU renderer on all cores, background compiled fragment path. | Dark Moon playable at 1x in CPU mode on the Thor. Pixel tests match the reference. |
| **6. Feature parity** | Custom textures, stereo 3D, save states, cheats, Amiibo, multiplayer, Artic Base, camera and microphone, per-game settings, device table entries. | Azahar feature checklist complete. |
| **7. Guest cores on threads, fast memory** | Thread-safe kernel, guest system and New 3DS cores on their own threads, fast host memory mapping. | New 3DS title tests; no regressions in Accurate mode. |
| **8. Tuning for the Duo** | Persistent translation cache, profile-guided warm-up, per-device defaults. | Duo targets met (Section 13). |

Phases 0–2 produce the first build where the no-pause and no-skip rules hold on a
real game. Phase 3 is where multicore speed is measured.

---

## 12. References

- **Azahar** (moving from GPLv2-or-later to GPLv3-or-later; 2126.2 already contains
  GPLv3-or-later files): source of the reused parts. The rebuild is GPLv3 or
  later as a result.
- **Citra MMJ:** the community considers it strong on mid-range and Mali devices.
  It uses OpenGL ES, where framebuffer fetch gives in-shader blending (method C in
  Section 4.2). Its distributed binaries reportedly include code that is not in its
  public source repository, so only the published `weihuoya/citra` source is consulted,
  for ideas only.
- **Dolphin's ubershaders and Yuzu's asynchronous GPU** are prior art for Sections 4
  and 5.

---

## 13. Targets, risks and limits

**Targets, confirmed per phase on your hardware:**

- **Every device:** zero compile pauses and zero skipped draws on cold first runs.
- **Thor:** Dark Moon opening at 100% at 2x in Hybrid mode.
- **Retroid Pocket Duo:** 100% at 1x in Hybrid mode as the floor; 2x is the stretch
  goal.

These are goals to test, not promises. The serial guest CPU stream sets a ceiling no
design can remove.

| Risk | Mitigation |
|---|---|
| The Duo's driver lacks features methods A or C need | Method B works on any Vulkan driver; Phase 0 measures its preparation time. |
| First-launch preparation is too long | All cores compile in parallel, the cache persists, and the variant set is reduced by method A or C where available. Measured in Phase 0. |
| Asynchronous GPU breaks a game's timing | Per-game Accurate mode, the same ordering as Azahar. |
| Thread-safe kernel work is large | Deferred to Phase 7; most games are limited by the main stream. |
| Scope and time | Reuse, line budgets, and a working APK at every phase. |
| Model strings for the Duo/DDS unknown | Generic Azahar behavior until a log from a real unit confirms them. |

---

## 14. Decisions (owner, 2026-10-14)

1. **Repository:** `RegiRex/claudhar`, seeded from Azahar 2126.2. Work happens on a
   `claude/` branch and is merged into `main` when a phase's work is complete and CI
   is green.
2. **Platform scope:** Android ARM64 only. Linux builds are used for CI tests and the
   probe tool. Windows, macOS, OpenGL and the Qt desktop app are not built.
3. **Test devices:** AYN Thor now; Retroid Pocket Duo expected within one to two
   weeks. Until then, choices made from the Thor's report keep method B (Section 4.2)
   available, since the Duo's Adreno 722 may lack features the Thor's Adreno 740 has.
   The Duo confirms the minimum targets when it arrives. The RG DDS stays in the
   device table plan but has no tester yet.
4. **Preparation screen:** accepted. A one-time "Preparing graphics" step on first
   launch and after app or driver updates, in exchange for no pauses during play.

---

## Sources

- Retroid Pocket Duo: [Android Authority](https://www.androidauthority.com/retroid-pocket-duo-specs-pricing-3710136/), [Retro Handhelds](https://retrohandhelds.gg/retroid-pocket-duo-everything-you-need-to-know/), [Retro Catalog](https://retrocatalog.com/retro-handhelds/retroid-pocket-duo), [Retroid store page](https://www.goretroid.com/products/retroid-pocket-duo-handheld)
- AYN Thor: [Android Authority](https://www.androidauthority.com/ayn-thor-screen-size-3589297), [Retro Dodo](https://retrododo.com/ayn-thor-pricing-specs-release-date-colours-officially-announced/)
- Anbernic RG DDS: [Anbernic](https://anbernic.com/blogs/news/anbernic-rg-dds-first-look), [Notebookcheck](https://www.notebookcheck.net/RG-DDS-Anbernic-reveals-new-Nintendo-3DS-inspired-gaming-handheld-with-500-nit-displays.1420177.0.html), [TechMyMoney](https://techmymoney.com/2026/10/11/anbernic-rg-dds-dual-screen-handheld-3ds-screens/)
- Adreno 722 and Turnip: [Banners-Turnip](https://github.com/The412Banner/Banners-Turnip), [Pocket Gaming Turnip guide](https://pocket-gaming.org/2026/06/15/the-definitive-guide-to-android-turnip-drivers-hardware-compatibility-2026/)
- Citra MMJ: [weihuoya/citra](https://awesome.ecosyste.ms/projects/github.com/weihuoya/citra), [Emulation General Wiki](https://emulation.gametechwiki.com/index.php/Citra)
- Azahar dual-screen and controller handling: `src/android/.../display/SecondaryDisplay.kt`, `fragments/AutoMapDialogFragment.kt`, `utils/ControllerMappingHelper.kt` in this repository.
