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

## AYN Thor (probe v2, 2026-10-28)

Report: [2026-10-28_thor_probe_v2.txt](device_reports/2026-10-28_thor_probe_v2.txt)

| Measurement | Result |
| --- | --- |
| Cold compile, one pipeline | ubershader 46 ms, compact ubershader 33 ms, specialized 21 ms |
| New blend variants of a compiled shader, compiles refused | all 64 built, none needed a compile, 0.19–0.38 ms each |
| Distinct specialized shaders on all 8 cores | 162 per second (median 43 ms each while 8 compile at once) |
| Next launch from the saved cache, compiles refused | all 64 built, 12 ms in total |

| Per-pixel cost, 800×480, 16 layers | ns per pixel | against specialized |
| --- | ---: | ---: |
| Switch-based ubershader | 1.26 | 6.8× |
| Compact ubershader (indexed sources, weighted-sum combiners) | 2.48 | 13.3× |
| Specialized shader | 0.19 | 1.0× |

No shader did work at its first draw, and all three images matched. The two runs of
the switch-based ubershader (2.15 and 1.26 ns per pixel) show that single runs vary;
the ratio to the specialized shader stays in the 7–9× range.

## Crash found on the way

Builds 4–9 crashed every game in `Vulkan::CreateSurface`
([reports](device_reports/2026-10-27_thor_crash_fea.txt)). The probe had added a second
definition of vulkan-hpp's inline dispatcher `init()`, compiled without the Android
platform macro, and the linker kept it for the renderer too, so the renderer never
loaded `vkCreateAndroidSurfaceKHR`. Fixed in build 10; CI now rejects any second
definition (`tools/claudhar/check_vulkan_dispatch.py`).

## Decisions

1. **Blending: method B with on-demand variants that cannot compile.** Confirmed on the
   Thor: once a shader is compiled, a pipeline that differs only in blending or other
   fixed state is created in well under a millisecond, from the cache, with
   `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`. Only shader programs
   need preparing; blend variants are created when first needed and saved to the
   pipeline cache. A refusal is a bug, counted in `compile_waits`, never a wait.
2. **Fragment shading: an always-ready ubershader plus background specialization.**
   The switch-based ubershader draws every configuration with no compile, at about
   1.3–2.2 ns per pixel on the Thor: roughly 3–4 ms of GPU time for a 2x frame with
   typical overdraw, which is workable at 2x and too slow for 4x on its own. Specialized
   shaders, 7–9 times cheaper per pixel, are compiled on worker threads (162 per second
   on all cores) and swapped in when ready, and the pipeline cache keeps them for the
   next launch. Nothing ever waits for them. The compact formulation is rejected: it is
   twice as slow as the switch-based one on the Adreno 740.
3. **Threads.** The guest CPU runs on the X3. GPU command processing, the Vulkan
   recorder and job workers run on the four A715/A710 cores. Audio, presentation and
   I/O run on the A510s. Job-system work items are small and taken dynamically, so the
   slower cores end up with less work instead of an equal share.
4. **Phase order.** The ubershader and background specialization are what the premier
   rule needs, and they work inside Azahar's existing Vulkan renderer. They move to
   Phase 1, so cold first runs stop pausing before the core is rewritten; the new core
   and renderer later reuse the same ubershader. See the plan's phase table.
