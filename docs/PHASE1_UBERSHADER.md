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

## Counters

Every 10 seconds the log has two lines like

```
Claudhar shading: requested 120654, drawn 120344 (specialized GPU-vertex 101200, specialized
CPU-vertex 15022, ubershader GPU-vertex 3890, ubershader CPU-vertex 232), empty 310,
invalid 0, CPU-vertex fallbacks 120, building: specialized 4, GPU-vertex ubershader 0,
compile_waits 0, skipped 0
Claudhar timing: 600 frames in 10.0 s, slowest 21 ms, 0 over 50 ms; CPU vertex shading of
fallbacks 35 ms; pipelines created on the emulation thread 41 in 9.6 ms, slowest 0.4 ms
```

`compile_waits` and `skipped` must be 0. Every draw the renderer is asked for is counted
as requested and again by its outcome; `skipped` is the difference, so a draw dropped by
any path shows up there. A compile wait also logs the pipeline state the driver could not
create from its cache, so the preparation can be extended to cover it. `CPU-vertex
fallbacks` counts GPU-vertex draws shaded on the CPU because no pipeline for them was
built yet; each is requested and drawn once, on the CPU. `building` counts the pipelines
queued on worker threads and not finished.

The timing line covers the same 10 seconds and shows what slows the game down without
breaking a rule: frames (from one presented frame to the next; pausing the emulation shows
as one long frame), the time the emulation thread spent shading fallback draws' vertices,
and pipelines it created itself from the driver cache, which should each take well under a
millisecond.

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
