// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Where the emulation thread's frame time goes. Until Phase 2 moves work to other threads,
// that one thread runs the whole 3DS, so its frame time sets the emulation speed. The time
// is summed over each log interval and logged as averages per frame, after the draw
// ledger's timings, split into:
//   - GPU commands: the command lists, memory fills, display transfers, texture copies and
//     DMA the game asks the 3DS GPU for, which Azahar carries out when they are asked for,
//     including shading vertices on the CPU;
//   - presenting: showing the finished frame and the renderer's end-of-frame work, apart
//     from the frame limiter's sleep, which is time to spare;
//   - the rest: the guest CPU, with the kernel and the system services it calls.
// Waits inside those phases are timed too: for the host GPU to finish submitted work
// (texture downloads, full stream buffers), for the Vulkan worker thread to record the
// commands queued so far (once per frame), and for a presentation frame to render into. Render
// passes, texture uploads and downloads are counted, since on tiled phone GPUs every render
// pass loads and stores its targets, which costs more the higher the resolution.
//
// Everything is counted on the emulation thread. A timed step costs two clock reads; nothing
// is timed per vertex or per pixel.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

#include "common/common_types.h"

namespace Claudhar::Diagnostics {

/// Parts of a frame. CpuVertices is inside GpuCommands and FrameLimiter inside Presenting;
/// the summary subtracts them where needed.
enum class Phase : u8 {
    GpuCommands,
    CpuVertices,
    Presenting,
    FrameLimiter,
    Count,
};

/// What the emulation thread waits for.
enum class WaitFor : u8 {
    Gpu,
    VulkanWorker,
    PresentFrame,
    Count,
};

class FrameProfile {
public:
    using Clock = std::chrono::steady_clock;

    void Add(Phase phase, Clock::duration time) {
        phases[static_cast<std::size_t>(phase)] += time;
    }

    void AddWait(WaitFor what, Clock::duration time) {
        waits[static_cast<std::size_t>(what)] += time;
        ++wait_counts[static_cast<std::size_t>(what)];
    }

    /// A render pass began; `same_target` when it draws into the same framebuffer as the
    /// pass before it, which ended for a transfer or a different draw area.
    void CountRenderPass(bool same_target) {
        ++render_passes;
        same_target_passes += same_target;
    }

    void CountVertices(u32 count) {
        cpu_vertices += count;
    }

    void CountUpload() {
        ++uploads;
    }

    void CountDownload(u64 bytes) {
        ++downloads;
        download_bytes += bytes;
    }

    /// One log line of averages over the `frames` frames that together took `frame_time`
    /// (the draw ledger's figures for the same interval), then starts the next interval.
    std::string TakeSummary(u32 frames, Clock::duration frame_time);

    /// Starts an interval without a summary, at the first frame, leaving out the loading.
    void Reset() {
        *this = FrameProfile{};
    }

private:
    std::array<Clock::duration, static_cast<std::size_t>(Phase::Count)> phases{};
    std::array<Clock::duration, static_cast<std::size_t>(WaitFor::Count)> waits{};
    std::array<u32, static_cast<std::size_t>(WaitFor::Count)> wait_counts{};
    u64 cpu_vertices = 0;
    u32 render_passes = 0;
    u32 same_target_passes = 0;
    u32 uploads = 0;
    u32 downloads = 0;
    u64 download_bytes = 0;
};

/// The emulation thread's profile. Emulation thread only.
FrameProfile& Profile();

/// Adds the time from its construction to its destruction to a phase.
class ScopedPhase {
public:
    explicit ScopedPhase(Phase phase_) : phase{phase_}, start{FrameProfile::Clock::now()} {}
    ~ScopedPhase() {
        Profile().Add(phase, FrameProfile::Clock::now() - start);
    }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;

private:
    Phase phase;
    FrameProfile::Clock::time_point start;
};

/// Adds the time from its construction to its destruction as one wait.
class ScopedWait {
public:
    explicit ScopedWait(WaitFor what_) : what{what_}, start{FrameProfile::Clock::now()} {}
    ~ScopedWait() {
        Profile().AddWait(what, FrameProfile::Clock::now() - start);
    }
    ScopedWait(const ScopedWait&) = delete;
    ScopedWait& operator=(const ScopedWait&) = delete;

private:
    WaitFor what;
    FrameProfile::Clock::time_point start;
};

} // namespace Claudhar::Diagnostics
