// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// The draw ledger: counters that show, in every log, that the premier rules held. Each
// guest draw ends in exactly one outcome (drawn by one of the three fragment paths, empty
// or invalid); compile waits and skipped draws must stay 0. The Vulkan renderer counts on
// the emulation thread and logs a summary every few seconds from TickFrame, so the
// counters cost one increment per draw and no logging in the draw path.

#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include "common/common_types.h"

namespace Claudhar::Shading {

struct DrawLedger {
    // Outcomes. Every draw lands in exactly one of these.
    u64 specialized_gpu_vertices = 0; ///< Specialized pipeline, vertices shaded on the GPU.
    u64 specialized_cpu_vertices = 0; ///< Specialized pipeline, vertices shaded on the CPU.
    u64 ubershader = 0;               ///< Ubershader pipeline, vertices shaded on the CPU.
    u64 empty = 0;                    ///< Nothing to draw: no render target, or no triangles left.
    u64 invalid = 0; ///< Invalid vertex data; drawn by neither Azahar nor Claudhar.

    // Events, not outcomes.
    u64 cpu_vertex_fallbacks = 0; ///< GPU-vertex draws whose pipeline was not built yet.
    u64 compile_waits = 0;        ///< Times the emulation thread waited for a compile. Must be 0.
    u64 skipped = 0;              ///< Draws dropped without an outcome above. Must be 0.

    /// Background pipeline builds queued and not finished; written by worker threads.
    std::atomic<u32> building{0};

    /// Total draws with an outcome.
    [[nodiscard]] u64 Drawn() const {
        return specialized_gpu_vertices + specialized_cpu_vertices + ubershader;
    }

    /// One log line with every counter.
    [[nodiscard]] std::string Summary() const;

    /// Logs the summary at most every `interval` while draws happen, and always when a
    /// rule counter (compile waits, skipped) changed. Called once per frame.
    void LogPeriodically(std::chrono::seconds interval = std::chrono::seconds{10});

private:
    std::chrono::steady_clock::time_point last_log{};
    u64 logged_drawn = 0;
    u64 logged_rule_breaks = 0;
};

} // namespace Claudhar::Shading
