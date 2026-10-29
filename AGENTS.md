# Claudhar development rules

These rules apply to every agent and contributor working in this repository.
The design and its phases are in [docs/PLAN.md](docs/PLAN.md).

## Premier rules

- **No shader or pipeline compile may ever be waited on during play, and no draw
  may ever be skipped.** Every draw runs once, in order. The `compile_waits` and
  `skipped` counters must stay 0 in tests and device logs; a nonzero value is a
  bug, never an acceptable slow path. Do not hide work by dropping it.
- Presentation may repeat or drop *displayed* frames for pacing; guest rendering
  never skips. Keep the two separate in counters and logs.

## Scope and hardware

- Android ARM64 only. Linux builds exist for CI tests and the probe tool.
- The Retroid Pocket Duo is the minimum target hardware; the AYN Thor is the
  primary test device. Decide on Vulkan features and thread placement from device
  probe reports, not assumptions about a chip or driver.

## Branches and builds

- Work on a `claude/` branch. Merge it into `main` when a phase's work is complete
  and its CI run is green. Never push to Azahar's repository or to `RegiRex/uberhar`.
- `.github/workflows/claudhar.yml` builds the APK as a direct download and runs the
  C++ tests. Only call a build ready when both of its jobs are green.
- Versions are `full.beta.alpha`. To publish an alpha, commit its notes as
  `docs/releases/<version>.md` (for example `docs/releases/0.0.2-alpha.md`), push the
  branch, and run the Claudhar workflow by hand on that branch with `alpha` set to the
  version. The run builds and tests the branch's newest commit and, when both jobs are
  green, publishes a GitHub prerelease with the APK and creates the tag on that commit.
  Pushing a version tag does the same; never push `--tags`, because the clone holds
  Azahar's own tags. Link files in release notes at the version tag, not `main`, which
  may not have them yet. Publish an alpha when a build is worth testing on a device; beta
  status needs device evidence, not a count of builds.

## Code

- C++20 for the core, Kotlin for the app, Vulkan only. New modules live under
  `src/claudhar/`, one directory per module. Reuse Azahar code that decides only
  *what* the 3DS does through thin adapters instead of rewriting it.
- **Comments on all written code.** Every new file starts with a purpose paragraph.
  Every function says what it does, which thread calls it and what it touches. Every
  non-obvious block says why. A change inside an Azahar file is marked `Claudhar:`
  with its reason, including why replaced behavior changed.
- **Lean code.** Stay within the module line budgets in the plan. One way to do each
  thing; no parallel experimental paths in hot loops. No per-draw allocation,
  per-pixel virtual calls or logging in hot loops.
- Format C++ with the repository's `.clang-format` and Kotlin with ktlint; no
  trailing whitespace.
- Fill a Vulkan dispatcher only through `Vulkan::InitDispatcher`, never with
  `DispatchLoaderDynamic::init` outside Azahar's Vulkan setup files: a second, different
  definition of that inline function once crashed every game. CI checks this.

## Licensing (GPL-3.0-or-later, following Azahar)

- Claudhar is distributed under GPLv3 or later (LICENSE.txt), as Azahar now is.
  Inherited files that say "GPLv2 or any later version" keep that header.
- A new file starts with:
  ```
  // Copyright <year> Claudhar Project
  // Licensed under GPLv3 or any later version
  // Refer to the LICENSE.txt file included.
  ```
- A modified Azahar file keeps its original header and adds, below it:
  `// Modified by the Claudhar Project in <year>; changes are marked "Claudhar:".`
- `tools/claudhar/check_license_headers.py` enforces both in CI. Never remove or
  change upstream copyright notices.

## Measuring

- Each change lands on its own with its measured effect. Keep it only if it helps on
  hardware. The test scenes are the Luigi's Mansion: Dark Moon opening (File 1
  reset, the title's shader cache deleted) and Fire Emblem Awakening, compared with
  stock Azahar on the same device and settings.
- Report host tests, CI results and device results separately. A green build or a
  passing host test does not show that a game runs faster or correctly.
- Device runs use turbo (no speed limit), so a log shows the highest speed a game
  reaches and how far it falls when something gets expensive. Analyze turbo intervals;
  do not exclude them. Judge a change by both numbers. A fall from 300% to 200% still
  plays at full speed and can be expected in demanding scenes. A fall to around 30%,
  from turbo or from 100%, is a failure. A fall that large under turbo points at
  Claudhar's own processing rather than at the scene's demand.
