# Phase 1: zero-pause shading in Azahar's renderer

Phase 1 removes shader compile pauses and skipped draws from Azahar's Vulkan renderer
without rewriting it. This note explains the design, what is proven and how, and what is
left to measure on the Thor.

## What changed

**Before.** Azahar builds one fragment shader per 3DS fragment configuration, and one
pipeline per shader and fixed-state combination, the first time a draw needs it. With
asynchronous shaders off (the default) the draw waits for the compile: a visible pause.
With them on, draws with more than six vertices are skipped until the pipeline is ready.

**Now.** No draw waits and none is skipped:

| Draw | Pipeline built | Pipeline not built yet |
| --- | --- | --- |
| Vertices shaded on the CPU | Azahar's specialized pipeline | The **fragment ubershader**; the specialized pipeline is compiled on a worker thread meanwhile |
| Vertices shaded on the GPU (hardware shader setting) | Azahar's specialized pipeline | The draw's own vertex shader with the **fragment ubershader**. That pipeline needs one compile per vertex shader, on workers of its own; until it is built, the vertices are shaded on the CPU and drawn as in the row above |

The ubershader is one GLSL fragment shader that reads the fragment configuration from a
uniform buffer (binding 6) instead of having it compiled in. A few configuration features
change what the shader declares (the kind of texture unit 0, shadow-map rendering, custom
normal maps), so there are up to 16 variants. While the game loads, every variant is
compiled on all cores, and its pipeline is created for each render target format pair.
Those pipelines are kept in their own driver pipeline cache file
(`shaders/vulkan/pipeline/claudhar-ubershader-*.bin`), which is not tied to a game, so
later launches create them without compiling. Pipelines that differ from those only in
other fixed state (blending, write mask, logic op) are then created during play with
`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`: Phase 0 measured these at
0.2–0.4 ms on the Thor, from the cache, with no compile. If the driver ever refuses one,
it is compiled at once and counted as a compile wait.

GPU-vertex draws needed the extra row in 0.0.2-alpha. In 0.0.1-alpha they went to the CPU
until their specialized pipeline was built, and on the Thor that was 97% of Dark Moon's
first draws (see the device test below): Azahar's CPU vertex path, which Uberhar measured
taking most of the frame in the same opening, cannot carry them. A specialized pipeline
is needed for every fragment configuration a vertex shader is drawn with; the ubershader
pipeline only once per vertex shader, and later fixed-state variants of it come from the
driver cache. Specialized pipelines are compiled at low thread priority, so they give way
to the emulation thread when both want the same core.

Azahar's "asynchronous shader compilation" setting no longer has any effect on Vulkan; it
chose between waiting and skipping, and neither happens now.

Code: `src/claudhar/shading/fs_ubershader.*` (generator and configuration buffer),
`src/claudhar/shading/draw_ledger.*` (counters), and changes marked `Claudhar:` in
`vk_pipeline_cache.*`, `vk_graphics_pipeline.*`, `vk_rasterizer.*` and
`vk_shader_disk_cache.*`.

### Driver caches, preparing before play and quick builds (0.0.5-alpha)

- **One driver pipeline cache per GPU driver for each game.** A game's driver cache was
  named after the game and the GPU model only, and a driver that found another driver's
  file deleted it, so moving a game between drivers (0.0.4-alpha lets each game have its
  own) compiled all its pipelines again. Each driver now has its own file
  (`<title>-<GPU>-<driver>.bin`). The files of the four most recently used drivers are
  kept, in the order a `<title>-<GPU>.drivers` list records, since file times are not
  reliable through Android's storage access. A file from before 0.0.5 is renamed after
  the driver that wrote it, from its header. Two drivers that support different features
  still make the game's shader record regenerate its SPIR-V at the loading screen when the
  game moves between them; the pipelines are kept.
- **Prepare graphics before starting** (off by default, per game). Pipelines recorded in
  earlier sessions that the driver cache cannot supply are built on all cores at the
  loading screen, with a progress count, instead of in the background while the
  ubershader draws. The loading screen only waits when there is something to build: after
  a driver change, an update or a cleared driver cache. Nothing changes during play. The
  log says `Claudhar: built N pipelines before starting in T ms`.
- **Quick shader builds** (a test setting in 0.0.5 only). A specialized pipeline missing
  from the driver cache was first built with `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`,
  then optimized on threads of its own, the optimized pipeline replacing the quick one
  between draws. The Thor showed it does not pay on either driver (see the device test
  below): quick builds took longer than the optimized ones, ubershader draws did not fall,
  and on Qualcomm's driver the extra compiles slowed the builds that matter most. 0.0.6
  removes it.

These settings are in the Testing section of the settings, with the game's GPU driver and
the internal resolution.

### Exit, build order and lookups (0.0.6-alpha)

- **Leaving a game no longer waits for queued builds.** The pipeline cache waited for
  every queued background build when the renderer shut down, while the frontend held the
  lock the UI thread needs when the game's window goes away. With Dark Moon on Qualcomm's
  driver, dozens of builds of several seconds each were queued, and Android reported the
  app as not responding. Queued builds are now dropped (they are recorded in the disk
  cache and built at the next start), and the running ones are waited for before
  shutdown takes that lock (`PipelineCache::StopBackgroundWork`, called through
  `RasterizerInterface::StopBackgroundWork` by the Android frontend).
- **GPU-vertex ubershader pipelines first.** A GPU-vertex draw's specialized pipeline is
  queued only once its GPU-vertex ubershader pipeline is built. Both compile the draw's
  vertex shader; only the ubershader one takes the draw off the CPU, and queued together
  it waited behind the specialized builds. Those pipelines also get as many threads as the
  specialized ones (four on the Thor, two before).
- **One driver cache lookup per pipeline.** A pipeline the driver cache turned down is not
  looked up again on the emulation thread; only its background build can make it.

## Counters

Every 10 seconds the log has two lines like

```
Claudhar shading: requested 120654, drawn 120344 (specialized GPU-vertex 101200, specialized
CPU-vertex 15022, ubershader GPU-vertex 3890, ubershader CPU-vertex 232), empty 310,
invalid 0, CPU-vertex fallbacks 120, building: specialized 4, GPU-vertex ubershader 0,
compile_waits 0, skipped 0
Claudhar timing: 897 frames in 10.0 s, speed 150%, slowest second 121%; slowest frame 21
ms, 0 over 50 ms; CPU vertex shading of fallbacks 35 ms; pipeline cache lookups on the
emulation thread 41 in 9.6 ms, 0 over 2 ms, slowest 0.4 ms
Claudhar memory: GPU 1843 MiB in use of a 11456 MiB budget
```

`compile_waits` and `skipped` must be 0. Every draw the renderer is asked for is counted
as requested and again by its outcome; `skipped` is the difference, so a draw dropped by
any path shows up there. A compile wait also logs the pipeline state the driver could not
create from its cache, so the preparation can be extended to cover it. `CPU-vertex
fallbacks` counts GPU-vertex draws shaded on the CPU because no pipeline for them was
built yet; each is requested and drawn once, on the CPU. `building` counts the pipelines
queued on worker threads and not finished.

The timing line covers the same 10 seconds and shows what slows the game down without
breaking a rule. The renderer presents a frame for every 3DS screen refresh (59.83 per
second), so frames per second give the emulation speed; the slowest whole second shows a
short slowdown that the average hides. Time the emulation spent paused is left out
(0.0.3-alpha; before, a pause showed as one long frame). Then come the time the emulation
thread spent shading fallback draws' vertices, and the pipelines it created itself from the
driver cache, which should each take well under a millisecond. The memory line gives the
GPU memory the process holds, from the driver (`VK_EXT_memory_budget`), or the renderer's
own allocations where the driver does not report it.

The log file is flushed whenever its queue runs empty (0.0.2-alpha), so after a crash it
ends where the game stopped. It used to be flushed only at errors.

## Matching Azahar's shaders

The ubershader has to draw what the specialized shader draws, or a surface would change
when its specialized pipeline swaps in. It follows the generator Azahar uses for the
configuration: the SPIR-V generator by default, the GLSL generator when that setting is
off or the configuration needs it (shadow-map rendering, shadow cube textures).

Reading both generators side by side showed three places where they disagree in meaning,
not only in rounding:

| Feature | SPIR-V generator | GLSL generator and the software renderer | Ubershader |
| --- | --- | --- | --- |
| Lighting with a primary shadow | Shadow also darkens the ambient term | Shadow darkens the diffuse term only | Diffuse only; **the SPIR-V generator is fixed to match** |
| Unknown color modifier (6, 7, 10, 11, 14, 15) | 1.0 | 0.0 | Follows the generator in use |
| Unknown texture unit 0 type (6, 7) | Black | Samples unit 0 as 2D | Follows the generator in use |
| Gas fog (unimplemented) | Writes the computed depth | Writes no depth | Follows the generator in use |

The SPIR-V generator also emitted invalid SPIR-V for procedural textures with a mipmap-
linear filter (a vec4 mix with a float result type); it is fixed.

**AddSigned now subtracts 128/255, in both generators and the ubershader.** It subtracted
0.5. With 8-bit inputs that puts every result exactly halfway between two 8-bit steps, and
GLSL leaves the direction of `round()` at a tie to the implementation, so the last bit of
the sum decided it. The driver orders the additions differently in the straight-line
specialized shader and in the ubershader's branches, so the two rounded differently on a
large share of pixels, by one step times the stage's scale. Azahar's software renderer, the
reference, computes `a + b - 128` in integers; 128/255 is that value, and it never ties.
Other combiner operations cannot tie, because their results are multiples of 1/255 and 255
is odd. This changes AddSigned results by at most one step from stock Azahar. For 2D shadow textures
the SPIR-V generator reads the lighting shadow binding while the GLSL generator reads
texture unit 0; they hold the same texture when the lighting shadow selects unit 0, which
is how games use it. The ubershader reads unit 0.

Rounding: where the SPIR-V generator writes a fused multiply-add the ubershader uses
`fma()`, and where the GLSL generator writes `a * b + c` it does too. Drivers may still
fuse other multiply-add pairs differently in the two shaders (compilers decide from the
shape of the surrounding code, which the ubershader's branches change), mostly in
lighting. Depth is a single expression in both and comes out bit-identical; a color can
differ by one 8-bit step on a pixel, scaled by later combiner stages.

One case is undefined in both shaders and cannot be made to match: a procedural texture
with a mipmap filter takes its level of detail from screen-space derivatives, and Vulkan
leaves those undefined next to pixels that the scissor test has already discarded. The two
shaders can then differ along the edge of the scissor box.

## How it is checked

`claudhar-ubershader-check` (run by CI on Mesa's lavapipe) renders random fragment
configurations, built field by field so every feature and invalid value Azahar tolerates
occurs, with the specialized shader and with the ubershader, and compares color, depth and
shadow maps. Each test passes when depth matches exactly wherever both shaders drew and at
most 2% of the pixels differ by more than one step or are discarded by only one shader
(an alpha test deciding the other way on a rounded alpha). `--minimize` reduces a failing
configuration to the fewest features that still fail.

Host result (lavapipe, LLVM 20; 2026-10-28): 8,000 random configurations, 4,000 through
each generator, all pass; 7,989 are pixel-identical, and the largest color difference is 4
steps, on one test. The run under the Khronos validation layers reports no errors.

These are host results. They do not show the Thor's driver behaves the same, how fast the
ubershader is in real games, or that cold first runs no longer pause; that is the device
test below.

## Device test

Cold first run of the Luigi's Mansion: Dark Moon opening (File 1 reset, the title's shader
cache deleted) on the Thor, against stock Azahar on the same settings:

- no visible shader hitches;
- `compile_waits 0` and `skipped 0` in the log;
- the log's ubershader and fallback counts, and speed through the opening.

### 0.0.1-alpha on the Thor (2026-10-29)

Fire Emblem Awakening and Dark Moon, Vulkan, 2x, in one app session. Every ledger line had
`compile_waits 0` and `skipped 0`; the ubershader was prepared in 1,194 ms the first time
and 127–152 ms from its cache afterwards. Graphics looked right in FEA. The phase is not
passed:

- **Slowdowns.** Speed fell well below 100%, with pauses, whenever new shaders were
  needed. In Dark Moon's first 12 seconds, 31,010 of 31,963 draws were GPU-vertex draws
  shaded on the CPU, with 58 specialized pipelines still queued. Fixed in 0.0.2-alpha as
  described above; the timing line will show what remains.
- **Crash** in Dark Moon soon after the opening cutscene began. The log ends 14 s into the
  game, at its last error line, because the file was flushed only at errors; the cause is
  not known yet. 0.0.2-alpha flushes the log continuously and waits for compile jobs before
  a title switch erases a shader cache they point into (a latent use-after-free, not shown
  to be this crash).
- **Flashes** of what looked like later scenes, briefly, when the game first loaded and
  just before the crash. Not explained yet; a comparison with stock Azahar on the same
  scene would show whether Claudhar causes them.

### 0.0.2-alpha on the Thor (2026-11-03)

Sonic & All-Stars Racing Transformed at 2x and Xenoblade Chronicles 3D at 2x, then at 4x,
with turbo (a 400% speed cap). Speeds below are frames per 10 seconds against the 3DS's
59.83 per second. Every ledger line had `compile_waits 0` and `skipped 0`.

- **GPU-vertex ubershader pipelines worked.** CPU-vertex fallbacks fell to 824 of 15.7
  million draws in Sonic and 636 of 19.4 million in Xenoblade at 2x; shading them took at
  most 69 ms in any 10 seconds.
- **Speed.** Sonic held 140–160%. Xenoblade at 2x ran between 86% and 350% per 10 seconds;
  at 4x between 66% and 208%, with three 10-second stretches holding 45–66 frames over
  50 ms, that is seconds near 30% speed. Those stretches had almost no fallbacks or
  pipeline creation, so the time went elsewhere; GPU load at 4x is the likely limit but GPU
  time is not measured. The owner's view: games that ran well ran almost perfectly, with
  highs well above the best Uberhar results and lows about at Uberhar's best lows.
- **Pipeline creation** from the driver cache on the emulation thread mostly took well
  under a millisecond, but up to 14–16 ms in a game's first seconds.
- **A 14-second "frame"** in Sonic was the app paused; 0.0.3-alpha leaves pauses out.
- **Crash** in Xenoblade at 4x about 160 seconds in: the driver refused a queue submit with
  `VK_ERROR_INITIALIZATION_FAILED`, which is not a result `vkQueueSubmit` may return and
  which stock Azahar does not catch either. The cause is not known; GPU memory running out
  is one possibility. 0.0.3-alpha logs GPU memory every 10 seconds and on a failed submit,
  retries the submit once after the GPU goes idle, and puts the end of the log into the
  crash report.

### 0.0.5-alpha on the Thor (2026-11-04)

Runs from a cold cache (the game's shader cache deleted first), with turbo. Every ledger
line had `compile_waits 0` and `skipped 0`.

- **Sonic at 4x on Turnip v27, a per-game driver,** in four runs with Prepare graphics
  before starting and Quick shader builds in each combination: 120–160% once racing, with
  no difference between the runs. Turnip built a specialized pipeline in 27–28 ms on
  average; a quick (unoptimized) build took 32–35 ms, longer. Ubershader draws over a run
  were 948 without quick builds and 861 and 1,044 with them.
- **Dark Moon at 2x on Qualcomm's driver (512.676):** 9–17% for as long as it was played,
  with quick builds on and off. Each pipeline with the game's vertex shaders took seconds
  to compile (quick builds 2.7–10 s on average, optimized 0.9–5 s); about 100 were queued
  in the first 10 seconds, and the GPU-vertex ubershader pipelines finished at 2–5 per 10
  seconds. Until a draw's pipeline is built its vertices are shaded on the CPU, which took
  7–8 s of every 10 s. Driver cache lookups on the emulation thread took up to 58 ms, and up
  to 1.3 s with quick builds on.
- **Fire Emblem Awakening at 4x on Qualcomm's driver:** 102–277%, with the slowest seconds
  at 33–50% while its pipelines were built.
- **The two "crashes" leaving Dark Moon were ANRs:** shutdown waited for the queued builds
  while holding the lock the UI thread needed. The crash reports also showed the next
  launch's log instead of the crashed session's. 0.0.6 fixes both.
- **Per-game drivers and per-driver caches worked:** Sonic loaded Turnip from its unpacked
  folder, and the 0.0.4 cache file was renamed after the Qualcomm driver that wrote it.
- **Prepare graphics before starting had nothing to do**, since every run began with the
  game's shader cache deleted.

What it means: in Dark Moon on Qualcomm's driver the vertex side is the limit. The
GPU-vertex ubershader pipelines that 0.0.2 added compile too slowly there to keep draws
off the CPU (in Sonic and Xenoblade they did; there, CPU-vertex draws were a few hundred in
millions). 0.0.6 builds those pipelines first and with more threads, and puts two Azahar
settings that change the size of the shaders the driver compiles (accurate multiplication
and the SPIR-V optimizer) in the Testing section. If that is not enough, the candidates are
a vertex shader interpreter on the GPU, which needs no compile per vertex shader (RPCS3 has
one for the PS3), and shading CPU-vertex draws on several cores, planned for Phase 3.
Turnip, which compiled Sonic's pipelines in about 30 ms, may also build Dark Moon's far
faster; that is a test to run.
