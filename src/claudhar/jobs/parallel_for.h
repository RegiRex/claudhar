// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Runs one loop on several cores and returns when every iteration is done. This is the seed
// of the job system Phase 3 plans (one system for every subsystem, sized from the device's
// cores); its one user so far is the PICA core, which shades the vertices of CPU-vertex draws
// with it.
//
// The loop's range is cut into chunks that the calling thread and a few long-lived worker
// threads claim one at a time, so fast and slow cores (the Thor and the Duo each mix cores a
// few times apart in speed) take as many chunks as they can finish. The caller works too and
// at the end waits only for the chunks still running. The workers sleep between loops.

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Claudhar::Jobs {

class ParallelFor {
public:
    /// Starts `workers` threads named after `name`. With none, Run runs on the caller alone.
    explicit ParallelFor(u32 workers, std::string_view name);
    ~ParallelFor();

    ParallelFor(const ParallelFor&) = delete;
    ParallelFor& operator=(const ParallelFor&) = delete;

    /// The threads a loop runs on: the workers and the caller.
    [[nodiscard]] u32 Threads() const {
        return static_cast<u32>(threads.size()) + 1;
    }

    /// Calls body(begin, end) for consecutive ranges of at most `chunk` items that together
    /// cover [0, count), on the workers and the calling thread, in no particular order, and
    /// returns when every call has returned. One thread calls Run at a time.
    template <typename Body>
    void Run(u32 count, u32 chunk, Body&& body) {
        RunErased(count, chunk, &Invoke<std::remove_reference_t<Body>>, &body);
    }

private:
    using Call = void (*)(void* body, u32 begin, u32 end);

    template <typename Body>
    static void Invoke(void* body, u32 begin, u32 end) {
        (*static_cast<Body*>(body))(begin, end);
    }

    void RunErased(u32 count, u32 chunk, Call call, void* body);

    /// Claims and runs chunks of the loop of `generation` until none is left.
    void Work(u32 generation);

    void WorkerLoop(std::stop_token token);

    // The current loop. RunErased writes them, under `mutex`, only while no worker is in
    // Work, and a worker enters Work only after taking `mutex`, so Work always reads one
    // loop's values. (A worker late from one loop, reading the next loop's chunk count while
    // the claim was still the old loop's, once claimed a chunk that did not exist.)
    Call call{};
    void* body{};
    u32 count{};
    u32 chunk{};
    u32 chunks{};

    /// The loop's generation in the high half and the next chunk to claim in the low half.
    std::atomic<u64> claim{0};
    /// Chunks of the current loop that have finished.
    std::atomic<u32> done{0};

    std::mutex mutex;
    std::condition_variable_any start_cv; ///< Wakes the workers for a new loop.
    /// Wakes the caller when the last chunk finishes or the last worker leaves Work.
    std::condition_variable done_cv;
    u32 generation{}; ///< The current loop's; under `mutex`.
    u32 active{};     ///< Workers in Work; under `mutex`.
    std::vector<std::jthread> threads;
};

} // namespace Claudhar::Jobs
