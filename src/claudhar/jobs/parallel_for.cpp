// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// The parallel loop; see parallel_for.h.

#include <algorithm>
#include <string>
#include <thread>
#include <fmt/format.h>

#include "claudhar/jobs/parallel_for.h"
#include "common/thread.h"

namespace Claudhar::Jobs {

namespace {
/// How often the caller yields while the last chunks finish on other cores before it
/// sleeps; a chunk is planned to take tens of microseconds, and waking costs about as much.
constexpr u32 FINISH_SPINS = 256;
} // namespace

ParallelFor::ParallelFor(u32 workers, std::string_view name) {
    threads.reserve(workers);
    for (u32 i = 0; i < workers; ++i) {
        threads.emplace_back(
            [this, thread_name = fmt::format("{} {}", name, i)](std::stop_token token) {
                Common::SetCurrentThreadName(thread_name.c_str());
                WorkerLoop(token);
            });
    }
}

ParallelFor::~ParallelFor() {
    // A sleeping worker wakes when its stop is requested; the threads are joined here.
    for (auto& thread : threads) {
        thread.request_stop();
    }
    threads.clear();
}

void ParallelFor::RunErased(u32 count_, u32 chunk_, Call call_, void* body_) {
    if (count_ == 0) {
        return;
    }
    chunk_ = std::max(chunk_, 1u);
    const u32 chunks_ = (count_ - 1) / chunk_ + 1;
    if (threads.empty() || chunks_ == 1) {
        call_(body_, 0, count_);
        return;
    }

    u32 current;
    {
        std::unique_lock lock{mutex};
        // A worker can still be on its way out of the previous loop, after its last chunk;
        // the fields must not change under it.
        done_cv.wait(lock, [&] { return active == 0; });
        call = call_;
        body = body_;
        count = count_;
        chunk = chunk_;
        chunks = chunks_;
        done.store(0, std::memory_order_relaxed);
        current = ++generation;
        claim.store(u64{current} << 32, std::memory_order_relaxed);
    }
    start_cv.notify_all();

    Work(current);

    // Only chunks other threads claimed can still be running; they are short.
    for (u32 spin = 0; spin < FINISH_SPINS; ++spin) {
        if (done.load(std::memory_order_acquire) == chunks_) {
            return;
        }
        std::this_thread::yield();
    }
    std::unique_lock lock{mutex};
    done_cv.wait(lock, [&] { return done.load(std::memory_order_acquire) == chunks_; });
}

void ParallelFor::Work(u32 current) {
    // The fields cannot change while this thread is here; see the header.
    u64 expected = claim.load(std::memory_order_relaxed);
    for (;;) {
        const u32 index = static_cast<u32>(expected);
        if (static_cast<u32>(expected >> 32) != current || index >= chunks) {
            return;
        }
        if (!claim.compare_exchange_weak(expected, expected + 1, std::memory_order_relaxed)) {
            continue; // `expected` now holds the current claim.
        }
        const u32 begin = index * chunk;
        call(body, begin, std::min(count, begin + chunk));
        // Release: the caller reads what the chunk wrote once it sees the loop done.
        if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == chunks) {
            // The last chunk of the loop: wake the caller if it went to sleep.
            std::scoped_lock lock{mutex};
            done_cv.notify_all();
        }
        expected = claim.load(std::memory_order_relaxed);
    }
}

void ParallelFor::WorkerLoop(std::stop_token token) {
    u32 seen = 0;
    while (!token.stop_requested()) {
        u32 current;
        {
            std::unique_lock lock{mutex};
            Common::CondvarWait(start_cv, lock, token, [&] { return generation != seen; });
            if (token.stop_requested()) {
                return;
            }
            current = seen = generation;
            ++active;
        }
        Work(current);
        std::scoped_lock lock{mutex};
        if (--active == 0) {
            done_cv.notify_all(); // RunErased may be waiting to start the next loop.
        }
    }
}

} // namespace Claudhar::Jobs
