// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Formats and logs the draw ledger; see draw_ledger.h.

#include <algorithm>
#include <fmt/format.h>

#include "claudhar/shading/draw_ledger.h"
#include "common/logging/log.h"

namespace Claudhar::Shading {

namespace {
/// A duration in milliseconds, for the log.
double Ms(DrawLedger::Clock::duration time) {
    return std::chrono::duration<double, std::milli>(time).count();
}
} // namespace

void DrawLedger::CountPipelineCreation(Clock::duration time) {
    ++pipeline_creations;
    pipeline_time += time;
    slowest_pipeline = std::max(slowest_pipeline, time);
}

void DrawLedger::CountFrame() {
    const auto now = Clock::now();
    if (last_frame == Clock::time_point{}) {
        last_log = now; // The first timings cover the first frames, not the loading.
    } else {
        const auto time = now - last_frame;
        ++frames;
        slow_frames += time > std::chrono::milliseconds{50};
        slowest_frame = std::max(slowest_frame, time);
    }
    last_frame = now;
}

std::string DrawLedger::Summary() const {
    return fmt::format(
        "requested {}, drawn {} (specialized GPU-vertex {}, specialized CPU-vertex {}, "
        "ubershader GPU-vertex {}, ubershader CPU-vertex {}), empty {}, invalid {}, "
        "CPU-vertex fallbacks {}, building: specialized {}, GPU-vertex ubershader {}, "
        "compile_waits {}, skipped {}",
        requested, Drawn(), specialized_gpu_vertices, specialized_cpu_vertices,
        ubershader_gpu_vertices, ubershader_cpu_vertices, empty, invalid, cpu_vertex_fallbacks,
        building.load(std::memory_order_relaxed),
        building_ubershaders.load(std::memory_order_relaxed), compile_waits, Skipped());
}

std::string DrawLedger::Timings(Clock::time_point now) const {
    return fmt::format("{} frames in {:.1f} s, slowest {:.0f} ms, {} over 50 ms; CPU vertex "
                       "shading of fallbacks {:.0f} ms; pipelines created on the emulation "
                       "thread {} in {:.1f} ms, slowest {:.1f} ms",
                       frames, Ms(now - last_log) / 1000.0, Ms(slowest_frame), slow_frames,
                       Ms(cpu_vertex_time), pipeline_creations, Ms(pipeline_time),
                       Ms(slowest_pipeline));
}

void DrawLedger::LogPeriodically(std::chrono::seconds interval) {
    const auto now = Clock::now();
    const u64 rule_breaks = compile_waits + Skipped();
    const bool rule_broken = rule_breaks != logged_rule_breaks;
    const bool due = Drawn() != logged_drawn && now - last_log >= interval;
    if (!rule_broken && !due) {
        return;
    }
    logged_rule_breaks = rule_breaks;
    // Claudhar log line: one per interval at most, plus one when a rule counter changes.
    if (rule_broken) {
        LOG_WARNING(Render_Vulkan, "Claudhar shading: {}", Summary());
    } else {
        LOG_INFO(Render_Vulkan, "Claudhar shading: {}", Summary());
    }
    if (!due) {
        return;
    }
    // Claudhar log line: the timings of the same interval.
    LOG_INFO(Render_Vulkan, "Claudhar timing: {}", Timings(now));
    last_log = now;
    logged_drawn = Drawn();
    frames = slow_frames = pipeline_creations = 0;
    slowest_frame = cpu_vertex_time = pipeline_time = slowest_pipeline = {};
}

} // namespace Claudhar::Shading
