// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// The draw ledger: counters that show, in every log, that the premier rules held. Each
// draw the renderer is asked for is counted once as requested and once by its outcome
// (drawn by one of the four pipeline kinds, empty or invalid). Skipped draws are the
// requests with no outcome, so a code path that drops a draw without saying so shows up
// there; they and compile waits must stay 0. The Vulkan renderer counts on the emulation
// thread and logs a summary every few seconds from TickFrame, so the counters cost a few
// increments per draw and no logging in the draw path.
//
// The ledger also times what can slow the emulation thread down without breaking a rule:
// long frames, shading the vertices of fallback draws on the CPU, and creating pipelines
// from the driver cache. Those timings cover the time since the last log line.

#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include "common/common_types.h"

namespace Claudhar::Shading {

struct DrawLedger {
    using Clock = std::chrono::steady_clock;

    /// Draws the renderer was asked for: GPU-vertex draws it accepted, and CPU-vertex
    /// batches (including the CPU retry of a GPU-vertex draw it did not accept).
    u64 requested = 0;

    // Outcomes. Every draw lands in exactly one of these.
    u64 specialized_gpu_vertices = 0; ///< Specialized pipeline, vertices shaded on the GPU.
    u64 specialized_cpu_vertices = 0; ///< Specialized pipeline, vertices shaded on the CPU.
    u64 ubershader_gpu_vertices = 0;  ///< Ubershader with the draw's GPU vertex shader.
    u64 ubershader_cpu_vertices = 0;  ///< Ubershader, vertices shaded on the CPU.
    u64 empty = 0;                    ///< Nothing to draw: no render target, or no triangles left.
    u64 invalid = 0; ///< Invalid vertex data; drawn by neither Azahar nor Claudhar.

    // Events, not outcomes.
    u64 cpu_vertex_fallbacks = 0; ///< GPU-vertex draws with no pipeline built yet.
    u64 compile_waits = 0;        ///< Times the emulation thread waited for a compile. Must be 0.

    /// Background pipeline builds queued and not finished, specialized and GPU-vertex
    /// ubershader ones; written by worker threads.
    std::atomic<u32> building{0};
    std::atomic<u32> building_ubershaders{0};

    /// Total draws drawn.
    [[nodiscard]] u64 Drawn() const {
        return specialized_gpu_vertices + specialized_cpu_vertices + ubershader_gpu_vertices +
               ubershader_cpu_vertices;
    }

    /// Requested draws with no outcome. Must be 0.
    [[nodiscard]] u64 Skipped() const {
        return requested - Drawn() - empty - invalid;
    }

    /// Adds the time the emulation thread spent shading a fallback draw's vertices.
    void CountCpuVertexTime(Clock::duration time) {
        cpu_vertex_time += time;
    }

    /// Adds one pipeline created on the emulation thread and the time it took.
    void CountPipelineCreation(Clock::duration time);

    /// Marks the end of a frame. Called once per frame from TickFrame.
    void CountFrame();

    /// One log line with every counter.
    [[nodiscard]] std::string Summary() const;

    /// Logs the summary and the timings at most every `interval` while draws happen, and
    /// the summary whenever a rule counter (compile waits, skipped) changed. Called once per
    /// frame, between draws, when every request has its outcome.
    void LogPeriodically(std::chrono::seconds interval = std::chrono::seconds{10});

private:
    /// One log line with the timings since the last one.
    [[nodiscard]] std::string Timings(Clock::time_point now) const;

    Clock::time_point last_log{};
    u64 logged_drawn = 0;
    u64 logged_rule_breaks = 0;

    // Timings since last_log.
    Clock::time_point last_frame{};
    u32 frames = 0;
    u32 slow_frames = 0; ///< Frames over 50 ms: a visible hitch.
    Clock::duration slowest_frame{};
    Clock::duration cpu_vertex_time{};
    u32 pipeline_creations = 0;
    Clock::duration pipeline_time{};
    Clock::duration slowest_pipeline{};
};

} // namespace Claudhar::Shading
