# Phase 0 findings

Device probe results and the design decisions taken from them. Raw reports are in
[device_reports](device_reports). The Retroid Pocket Duo's report is added when the
device arrives.

## AYN Thor (probe v1, 2026-10-15)

Report: [2026-10-15_thor_probe_v1.txt](device_reports/2026-10-15_thor_probe_v1.txt)

**Identity, for the known-device table.** Manufacturer `AYN`, model `AYN Thor`,
device and board `kalama`, SoC `QCS8550`, Android 13.

**Screens.** Display 0, "Built-in Screen", is 1080×1920 (a portrait-native panel) at
60 or 120 Hz. Display 4, "Screen-2", is 1080×1240 at 60 or 120 Hz and is a
presentation display. Azahar's generic secondary-display rule already picks it, so
the bottom screen needs no Thor-specific rule.

**Controls.** The built-in controller is "Odin Controller", vendor 0x2020, product
0x0111, reported as external. It has the standard gamepad buttons, sticks on X/Y and
Z/RZ, analog triggers on BRAKE/GAS and the d-pad on HAT_X/HAT_Y. The touch panels
are "fts_ts" (top) and "fts_ts_3" (bottom).

**CPU.** One Cortex-X3 at 3.19 GHz (relative speed 1.00), two A715 and two A710 at
2.80 GHz (0.90–0.91), three A510 at 2.02 GHz (0.31–0.33). This is the same 1 + 4 + 3
shape as the Duo. A little core does about a third of a big core's work.

**Vulkan.** Adreno 740, Vulkan 1.3.128, driver dated 2023-12-27. No dynamic blend
state (EDS3), no framebuffer-read extensions, no pipeline libraries, shader objects
or pipeline binaries. On the stock driver, method B is the only blending method.

**Pipeline costs.**

| Measurement | Result |
| --- | --- |
| Ubershader, cold compile | 41 ms |
| New blend variants after it | 0.15–0.32 ms each, all served from the pipeline cache |
| Saved cache for 64 variants | 20 KiB |
| Next launch from the saved cache, compiles refused | all 64 built, 13 ms in total |
| Same without the saved cache | all 64 needed a compile |

The application's pipeline cache is what makes a launch compile-free; the driver
keeps no cache of its own that would.

**Per-pixel cost.** The ubershader costs 2.15 ns per pixel against 0.23 ns for the
same material as a specialized shader: 9.25 times as much. The first run matched the
best run, so the driver does no compiling at first draw. Both images matched.

## Decisions

1. **Blending: method B, with on-demand variants that cannot compile.** Shader
   programs come from a finite set built on the "Preparing graphics" screen. Pipelines
   that differ only in blending or other fixed state are created when first needed,
   with `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`, so the driver
   either serves them from the cache in well under a millisecond or refuses. A refusal
   is a bug, counted in `compile_waits`, never a wait. New variants are saved to the
   cache for the next launch. Probe v2 tests "never compiles" directly.
2. **Fragment shading: one switch-based ubershader is too slow per pixel.** At a
   typical 2x frame (about 0.7 million pixels drawn about three times) it would take
   roughly 4–5 ms of the Thor's GPU time: workable at 2x on the Thor, too slow for 4x
   and marginal on the Duo. Probe v2 measures a compact, mostly branch-free ubershader.
   Phase 2 then chooses between that ubershader, a precompiled family of partially
   specialized shaders sized by the measured compile throughput, or both (family
   members for common configurations, the ubershader as the always-ready fallback).
   Combo mode keeps background specialization.
3. **Threads.** The guest CPU runs on the X3. GPU command processing, the Vulkan
   recorder and job workers run on the four A715/A710 cores. Audio, presentation and
   I/O run on the A510s. Job-system work items are small and taken dynamically, so the
   slower cores end up with less work instead of an equal share.

## Probe v2

Adds, for the Thor and later the Duo:

- the compact ubershader's per-pixel cost next to the other two, with an image check;
- new blend variants built with compiles refused, which shows directly that they
  never compile;
- the cold compile time of each shader kind;
- how many distinct shaders all cores compile per second, which sets how large a
  precompiled shader family can be.
