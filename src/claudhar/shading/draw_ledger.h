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
// from the driver cache. Those timings cover the time since the last log line, leaving out
// time the emulation spent paused. The renderer presents once per 3DS screen refresh, so
// frames per second of emulation time give the emulation speed.

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

    /// Quick builds, the Quick Shader Builds test setting: a specialized pipeline missing
    /// from the driver cache is first built without the driver's optimizations, so it takes
    /// over from the ubershader sooner, then built again optimized, which replaces it. The
    /// counters show whether that pays on a driver: how long each kind of build took, and how
    /// many draws used a quick build. Builds are counted by worker threads, draws by the
    /// emulation thread. The figures cover the whole session.
    struct QuickBuilds {
        bool enabled = false;           ///< The setting, read when the game loads.
        std::atomic<u32> quick{0};      ///< Quick builds finished.
        std::atomic<u32> optimized{0};  ///< Optimized builds that followed one.
        std::atomic<u32> optimizing{0}; ///< Optimized builds queued and not finished.
        std::atomic<u64> quick_ns{0};   ///< Time spent in quick builds.
        std::atomic<u64> optimized_ns{0};
        u64 draws = 0; ///< Draws drawn with a quick build.

        /// Adds a finished build of either kind and the time it took. Worker threads.
        void Count(bool is_quick, Clock::duration time) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time).count();
            (is_quick ? quick_ns : optimized_ns).fetch_add(ns, std::memory_order_relaxed);
            (is_quick ? quick : optimized).fetch_add(1, std::memory_order_relaxed);
        }
    } quick_builds;

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

    /// Adds one attempt to create a pipeline from the driver cache on the emulation thread,
    /// and the time it took.
    void CountPipelineCreation(Clock::duration time);

    /// Marks the end of a frame. Called once per frame from TickFrame.
    void CountFrame();

    /// One log line with every counter.
    [[nodiscard]] std::string Summary() const;

    /// Logs the summary and the timings at most every `interval` while draws happen, and
    /// the summary whenever a rule counter (compile waits, skipped) changed. Called once per
    /// frame, between draws, when every request has its outcome. Returns true when it logged
    /// the timings, so the caller can add its own figures for the same interval.
    bool LogPeriodically(std::chrono::seconds interval = std::chrono::seconds{10});

private:
    /// One log line with the timings since the last one.
    [[nodiscard]] std::string Timings() const;

    Clock::time_point last_log{};
    u64 logged_drawn = 0;
    u64 logged_rule_breaks = 0;

    // Timings since last_log.
    Clock::time_point last_frame{};
    u32 frames = 0;
    Clock::duration frame_time{}; ///< The frames' total: elapsed time without pauses.
    u32 slow_frames = 0;          ///< Frames over 50 ms: a visible hitch.
    Clock::duration slowest_frame{};
    u32 second_frames = 0;         ///< Frames in the current second of frame_time.
    Clock::duration second_time{}; ///< frame_time since the current second began.
    double slowest_second = 0.0;   ///< Lowest speed of a whole second, 0 before the first.
    Clock::duration cpu_vertex_time{};
    u32 pipeline_creations = 0;
    u32 slow_pipeline_creations = 0; ///< Those over 2 ms.
    Clock::duration pipeline_time{};
    Clock::duration slowest_pipeline{};
};

/// Tells the ledger that the emulation was paused, so the frame that spans the pause is
/// left out of the timings. Called by the frontend's emulation thread when it resumes.
void MarkEmulationPaused();

} // namespace Claudhar::Shading
