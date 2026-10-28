# Claudhar

**A ground-up rebuild of the Azahar 3DS emulator for Android, designed around
multi-core CPUs, with zero draw skipping and zero shader compilation pauses.**

[Download a test build](https://github.com/RegiRex/claudhar/actions/workflows/claudhar.yml) ·
[Report an issue](https://github.com/RegiRex/claudhar/issues) ·
[Rebuild plan](docs/PLAN.md)

> [!WARNING]
> Claudhar is starting **Phase 1**: today it is Azahar 2126.2 with a new name, its
> own app ID, a device probe and a crash reporter. Zero-pause shading comes next; the
> multi-core core and the new renderer follow. Do not expect it to run games better
> than Azahar yet.

## Goals

Two rules come before speed:

- **No shader compilation pauses, ever.** Nothing the renderer needs is compiled
  while a game runs. Everything is either built once before play, on a one-time
  "Preparing graphics" screen, or needs no compiling at all.
- **No skipped draws, ever.** Every draw the game asks for runs exactly once, in
  order. Counters prove it in every build, and tests fail when either rule breaks.

Then speed, from every core:

- The guest CPU, GPU command processing, geometry, rasterization, audio and
  presentation run on separate threads at the same time instead of taking turns
  on one thread, with one shared job system for parallel work.
- The **Retroid Pocket Duo** (eight cores, 8 GB) is the minimum hardware the
  design targets. The **AYN Thor** is the primary test device.

These are goals, not results. The [plan](docs/PLAN.md) explains the design,
the phases and what each phase must prove on real devices.

## Rendering modes (planned)

| Mode | Vertices | Pixels | Compiles during play |
| --- | --- | --- | --- |
| **Hybrid** | CPU, on all cores | GPU, one precompiled ubershader | None |
| **Combo** | CPU, or GPU for programs already built | GPU, swapping in shaders specialized in the background | Background only, never waited on |
| **CPU** | CPU | CPU tiles on every core; the GPU only presents | None |

Hybrid and Combo correspond to Uberhar's Native and Combo modes. Any mode can be
switched to **Accurate** timing per game, which processes GPU commands in step
with the game exactly as Azahar does.

## Installing a test build

1. Open the [Claudhar workflow runs](https://github.com/RegiRex/claudhar/actions/workflows/claudhar.yml)
   and pick the newest green run (both `native-tests` and `android` green).
2. Use the **Download the Claudhar APK** link at the top of the run page. It
   downloads the `.apk` itself; there is nothing to unzip. Downloads need a GitHub
   sign-in and expire after 30 days.
3. Install it on a **64-bit ARM Android device running Android 10 or newer**.
   Claudhar is a separate app (`io.github.regirex.claudhar`), so it installs next
   to Azahar and Uberhar without replacing either.
4. On first launch choose a new, empty data folder such as `Claudhar`. Copy keys,
   saves or system files into it if you want them.

If a run warns about a new signing key, uninstall the previous Claudhar build
before installing the new one.

## Device probe

Phase 0 measures each test device before the renderer is designed around it.
Open the **Options** tab and tap **Device probe** while no game is running. It takes up to a
minute and records:

- the device's model strings, displays and input devices, for automatic
  dual-screen and controller setup;
- every CPU core's type, frequency range and measured speed;
- the Vulkan driver's features for compile-free blending, how long pipeline
  builds take cold, warm and on all cores, and what the ubershader costs per
  pixel against a specialized shader.

The report is saved as `log/claudhar_probe.txt` in the data folder. Use **Share**
or **Copy** to send it.

## Building

```sh
git clone --recurse-submodules https://github.com/RegiRex/claudhar.git
cd claudhar
```

- **Android:** the [workflow](.github/workflows/claudhar.yml) holds the full
  recipe. In short, with JDK 17, Android SDK platform 35, NDK 27.3.13750724 and
  CMake 3.30.3 installed, run `./gradlew assembleVanillaRelease` in `src/android`.
- **Linux (tests and probe only):** configure with CMake and Ninja as the workflow's
  `native-tests` job does, then run `ctest`. `build/bin/Release/claudhar-probe`
  prints the native half of the device probe; with Mesa's lavapipe installed it runs
  without a GPU.

Windows, macOS, the Qt desktop app and OpenGL are inherited from Azahar but are not
built or supported. Claudhar targets Android ARM64 only.

## Contributing

Issues and pull requests are welcome at [RegiRex/claudhar](https://github.com/RegiRex/claudhar).
For a bug, include the build (run number or commit), the device and driver, the
game and scene, the mode, resolution and settings, and the log. A device probe
report helps for anything device-specific.

Code standards for this repository (comments on all new code, license notices,
line budgets, measuring every change) are in [AGENTS.md](AGENTS.md).

Claudhar is developed with an AI assistant (Claude). Azahar's
[AI use policy](docs/UPSTREAM_AI_POLICY_AZAHAR.md) does not accept AI-written
contributions, so Claudhar code must not be submitted to Azahar.

## Upstream, credits and license

Claudhar builds on **Azahar**, which brought together **PabloMK7's Citra fork** and
**Lime3DS**, and on the work of **Citra** and all of their contributors. Their
system services, file systems, loader, cryptography, audio, Android app and the
**Dynarmic** ARM translator are reused. The [original Azahar README](docs/UPSTREAM_README_AZAHAR.md)
is preserved with its history, platform notes and community links. Claudhar is an
independent project; Azahar's services and community do not support it.

Claudhar is licensed under **GPL-3.0-or-later**, following Azahar, which is moving
from GPLv2-or-later to GPLv3-or-later; see [LICENSE.txt](LICENSE.txt). Azahar 2126.2
already contains files licensed GPLv3 or later, so Claudhar as a whole is distributed
under GPLv3 or later. Most files taken from Azahar still say "GPLv2 or any later
version", which allows distribution under GPLv3; their headers are kept as they are.
New Claudhar files carry a Claudhar header with GPLv3 or later. Azahar files that
Claudhar changes add a "Modified by the Claudhar Project" notice and mark each change
`Claudhar:`. Bundled dependencies in `externals/` keep their own licenses.
