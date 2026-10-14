// Copyright 2026 Claudhar Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Phase 0 device probe. It measures what the rebuild's design choices depend on,
// on the device itself: how many cores of which speed the process may use, which
// Vulkan features the driver offers for compile-free rendering, how long pipeline
// builds take cold, warm and in parallel, and what a fragment ubershader costs
// against a specialized shader. Each function returns a plain-text report section
// that the Android probe screen and the Linux claudhar-probe tool print as is.

#pragma once

#include <string>

namespace Claudhar::Probe {

/// How much measuring to do. Quick keeps CI runs on a CPU Vulkan driver short.
struct Options {
    bool quick = false; ///< Fewer pipeline variants, layers and timing repeats.
};

/// CPU layout and speed: core types, frequencies, capacities, the cores this process
/// may run on, and a single-thread speed test pinned to each of them in turn.
/// Called on any thread; takes about one second on a phone (less when quick).
std::string ReportCpu(const Options& options);

/// Vulkan driver report: device and driver identity, the features that decide how
/// blending avoids runtime compiles, pipeline build timings and ubershader cost.
/// Called on any thread while no game runs; uses its own Vulkan instance and
/// dispatcher, so the emulator's renderer state is untouched. Never throws: a
/// failure is reported in the returned text.
std::string ReportVulkan(const Options& options);

} // namespace Claudhar::Probe
