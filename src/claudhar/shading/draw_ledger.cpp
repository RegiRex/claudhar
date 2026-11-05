// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Formats and logs the draw ledger; see draw_ledger.h.

#include <algorithm>
#include <atomic>
#include <fmt/format.h>

#include "claudhar/shading/draw_ledger.h"
#include "common/logging/log.h"

namespace Claudhar::Shading {

namespace {
/// The 3DS refreshes its screens every 4,481,136 cycles of its 268,111,856 Hz clock.
constexpr double REFRESH_RATE = 268111856.0 / 4481136.0;

/// Set by MarkEmulationPaused, taken by the next CountFrame.
std::atomic<bool> paused{false};

/// A duration in milliseconds, for the log.
double Ms(DrawLedger::Clock::duration time) {
    return std::chrono::duration<double, std::milli>(time).count();
}

/// The emulation speed in percent of `frames` presented in `time`.
double Speed(u32 frames, DrawLedger::Clock::duration time) {
    const double seconds = std::chrono::duration<double>(time).count();
    return seconds > 0.0 ? 100.0 * frames / (REFRESH_RATE * seconds) : 0.0;
}
} // namespace

void MarkEmulationPaused() {
    paused.store(true, std::memory_order_relaxed);
}

void DrawLedger::CountPipelineCreation(Clock::duration time) {
    ++pipeline_creations;
    slow_pipeline_creations += time > std::chrono::milliseconds{2};
    pipeline_time += time;
    slowest_pipeline = std::max(slowest_pipeline, time);
}

void DrawLedger::CountFrame() {
    const auto now = Clock::now();
    const bool first = last_frame == Clock::time_point{};
    if (first) {
        last_log = now; // The first timings cover the first frames, not the loading.
    }
    // A frame that spans a pause is not a hitch; leave it out.
    if (!paused.exchange(false, std::memory_order_relaxed) && !first) {
        const auto time = now - last_frame;
        ++frames;
        frame_time += time;
        slow_frames += time > std::chrono::milliseconds{50};
        slowest_frame = std::max(slowest_frame, time);
        // The slowest whole second shows a short slowdown that the average hides.
        ++second_frames;
        second_time += time;
        if (second_time >= std::chrono::seconds{1}) {
            const double speed = Speed(second_frames, second_time);
            slowest_second = slowest_second == 0.0 ? speed : std::min(slowest_second, speed);
            second_frames = 0;
            second_time = {};
        }
    }
    last_frame = now;
}

std::string DrawLedger::Summary() const {
    std::string summary =
        fmt::format("requested {}, drawn {} (specialized GPU-vertex {}, specialized CPU-vertex {}, "
                    "ubershader GPU-vertex {}, ubershader CPU-vertex {}), empty {}, invalid {}, "
                    "CPU-vertex fallbacks {}, building: specialized {}, GPU-vertex ubershader {}, "
                    "compile_waits {}, skipped {}",
                    requested, Drawn(), specialized_gpu_vertices, specialized_cpu_vertices,
                    ubershader_gpu_vertices, ubershader_cpu_vertices, empty, invalid,
                    cpu_vertex_fallbacks, building.load(std::memory_order_relaxed),
                    building_ubershaders.load(std::memory_order_relaxed), compile_waits, Skipped());
    // Only when the test setting is on, so the usual line stays as it was.
    if (quick_builds.enabled) {
        const auto average_ms = [](const std::atomic<u64>& ns, const std::atomic<u32>& count) {
            const u32 n = count.load(std::memory_order_relaxed);
            return n ? static_cast<double>(ns.load(std::memory_order_relaxed)) / 1e6 / n : 0.0;
        };
        summary += fmt::format(
            "; quick builds {} ({:.1f} ms each), optimized after them {} ({:.1f} ms each), "
            "optimizing {}, quick draws {}",
            quick_builds.quick.load(std::memory_order_relaxed),
            average_ms(quick_builds.quick_ns, quick_builds.quick),
            quick_builds.optimized.load(std::memory_order_relaxed),
            average_ms(quick_builds.optimized_ns, quick_builds.optimized),
            quick_builds.optimizing.load(std::memory_order_relaxed), quick_builds.draws);
    }
    return summary;
}

std::string DrawLedger::Timings() const {
    return fmt::format("{} frames in {:.1f} s, speed {:.0f}%, slowest second {:.0f}%; slowest "
                       "frame {:.0f} ms, {} over 50 ms; CPU vertex shading of fallbacks {:.0f} "
                       "ms; pipeline cache lookups on the emulation thread {} in {:.1f} ms, {} "
                       "over 2 ms, slowest {:.1f} ms",
                       frames, Ms(frame_time) / 1000.0, Speed(frames, frame_time), slowest_second,
                       Ms(slowest_frame), slow_frames, Ms(cpu_vertex_time), pipeline_creations,
                       Ms(pipeline_time), slow_pipeline_creations, Ms(slowest_pipeline));
}

bool DrawLedger::LogPeriodically(std::chrono::seconds interval) {
    const auto now = Clock::now();
    const u64 rule_breaks = compile_waits + Skipped();
    const bool rule_broken = rule_breaks != logged_rule_breaks;
    const bool due = Drawn() != logged_drawn && now - last_log >= interval;
    if (!rule_broken && !due) {
        return false;
    }
    logged_rule_breaks = rule_breaks;
    // Claudhar log line: one per interval at most, plus one when a rule counter changes.
    if (rule_broken) {
        LOG_WARNING(Render_Vulkan, "Claudhar shading: {}", Summary());
    } else {
        LOG_INFO(Render_Vulkan, "Claudhar shading: {}", Summary());
    }
    if (!due) {
        return false;
    }
    // Claudhar log line: the timings of the same interval.
    LOG_INFO(Render_Vulkan, "Claudhar timing: {}", Timings());
    last_log = now;
    logged_drawn = Drawn();
    frames = slow_frames = second_frames = pipeline_creations = slow_pipeline_creations = 0;
    frame_time = slowest_frame = second_time = cpu_vertex_time = pipeline_time =
        slowest_pipeline = {};
    slowest_second = 0.0;
    return true;
}

} // namespace Claudhar::Shading
