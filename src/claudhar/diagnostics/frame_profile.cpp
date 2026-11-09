// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Formats the frame profile; see frame_profile.h.

#include <algorithm>
#include <fmt/format.h>

#include "claudhar/diagnostics/frame_profile.h"

namespace Claudhar::Diagnostics {

FrameProfile& Profile() {
    static FrameProfile profile;
    return profile;
}

std::string FrameProfile::TakeSummary(u32 frames, Clock::duration frame_time) {
    const auto ms = [](Clock::duration time) {
        return std::chrono::duration<double, std::milli>(time).count();
    };
    const double n = std::max(frames, 1u);
    const auto phase = [&](Phase p) { return ms(phases[static_cast<std::size_t>(p)]); };
    const auto wait = [&](WaitFor w) { return ms(waits[static_cast<std::size_t>(w)]) / n; };
    const auto times = [&](WaitFor w) { return wait_counts[static_cast<std::size_t>(w)] / n; };

    // The phases are summed over the same frames as frame_time, so the rest of it is the
    // guest's. A frame left out of frame_time for spanning a pause can make that slightly
    // short, never negative.
    const double gpu = phase(Phase::GpuCommands);
    const double presenting = phase(Phase::Presenting);
    const double limiter = phase(Phase::FrameLimiter);
    const double guest = std::max(ms(frame_time) - gpu - presenting, 0.0);

    std::string summary = fmt::format(
        "{:.1f} ms per frame: guest CPU and services {:.1f}, GPU commands {:.1f} (CPU vertex "
        "shading {:.1f} for {:.0f} vertices), presenting {:.1f}, frame limiter {:.1f}; waits per "
        "frame for the GPU {:.2f} ms ({:.1f} times), the Vulkan worker {:.2f} ms, a free "
        "presentation frame {:.2f} ms; per frame {:.1f} render passes ({:.1f} on the same target "
        "as the one "
        "before), {:.1f} texture uploads, {:.2f} downloads ({:.0f} KiB)",
        ms(frame_time) / n, guest / n, gpu / n, phase(Phase::CpuVertices) / n, cpu_vertices / n,
        (presenting - limiter) / n, limiter / n, wait(WaitFor::Gpu), times(WaitFor::Gpu),
        wait(WaitFor::VulkanWorker), wait(WaitFor::PresentFrame), render_passes / n,
        same_target_passes / n, uploads / n, downloads / n, download_bytes / 1024.0 / n);
    Reset();
    return summary;
}

} // namespace Claudhar::Diagnostics
