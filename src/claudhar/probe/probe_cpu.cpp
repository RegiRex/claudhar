// Copyright 2026 Claudhar Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// CPU half of the device probe. Thread placement in the rebuild is decided from
// these numbers: which cores exist, how fast each one really is for emulator-like
// integer work, and which of them Android lets this process use.

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "claudhar/probe/probe.h"
#include "common/common_types.h"

#if defined(__linux__)
#include <sched.h>
#endif

namespace Claudhar::Probe {
namespace {

/// Reads the first line of a small text file, or returns "" if it is missing.
std::string ReadLine(const std::string& path) {
    std::ifstream file{path};
    std::string line;
    std::getline(file, line);
    return line;
}

/// Names the Arm core design from the "CPU part" number in /proc/cpuinfo.
std::string CoreName(u32 part) {
    static const std::map<u32, const char*> names{
        {0xd05, "Cortex-A55"},  {0xd0d, "Cortex-A77"},  {0xd41, "Cortex-A78"},
        {0xd44, "Cortex-X1"},   {0xd46, "Cortex-A510"}, {0xd47, "Cortex-A710"},
        {0xd48, "Cortex-X2"},   {0xd4d, "Cortex-A715"}, {0xd4e, "Cortex-X3"},
        {0xd80, "Cortex-A520"}, {0xd81, "Cortex-A720"}, {0xd82, "Cortex-X4"},
        {0xd85, "Cortex-X925"}, {0xd87, "Cortex-A725"},
    };
    const auto it = names.find(part);
    return it != names.end() ? it->second : fmt::format("part {:#x}", part);
}

/// Maps each processor number to its core design, from /proc/cpuinfo.
std::map<u32, std::string> CoreNames() {
    std::map<u32, std::string> result;
    std::ifstream file{"/proc/cpuinfo"};
    u32 processor = 0;
    for (std::string line; std::getline(file, line);) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string value = line.substr(colon + 1);
        if (line.starts_with("processor"))
            processor = static_cast<u32>(std::stoul(value));
        else if (line.starts_with("CPU part"))
            result[processor] = CoreName(static_cast<u32>(std::stoul(value, nullptr, 16)));
    }
    return result;
}

/// Emulator-like integer work: a dependent multiply chain with loads and stores
/// into a 16 KiB table, like a JIT reading guest state. Returns a checksum so the
/// compiler cannot drop the loop.
u64 Workload(u32 iterations) {
    std::array<u32, 4096> table{};
    u64 x = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < iterations; ++i) {
        x = x * 6364136223846793005ULL + table[x >> 52];
        table[(x >> 40) & 4095] ^= static_cast<u32>(x);
    }
    return x;
}

/// Runs the workload pinned to one CPU and returns the best time in microseconds,
/// or a negative value if this process may not run there. The warm-up gives the
/// governor time to raise that core's clock first.
double TimeOnCpu(u32 cpu, u32 iterations, u32 repeats) {
    double best = -1.0;
    std::thread worker([&] {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            return;
#endif
        volatile u64 sink = Workload(iterations); // Warm-up, not timed.
        for (u32 r = 0; r < repeats; ++r) {
            const auto start = std::chrono::steady_clock::now();
            sink = sink + Workload(iterations);
            const std::chrono::duration<double, std::micro> took =
                std::chrono::steady_clock::now() - start;
            best = best < 0.0 ? took.count() : std::min(best, took.count());
        }
    });
    worker.join();
    return best;
}

} // Anonymous namespace

std::string ReportCpu(const Options& options) {
    std::string out = "== CPU ==\n";
    const auto append = [&out](std::string line) { out += line + '\n'; };
    append(fmt::format("logical cpus: {}", std::thread::hardware_concurrency()));
    append(fmt::format("possible: {}  online: {}", ReadLine("/sys/devices/system/cpu/possible"),
                       ReadLine("/sys/devices/system/cpu/online")));

    // The cores Android's cpuset lets this process use; others are reported but
    // cannot be pinned to.
    std::vector<u32> allowed;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (u32 cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &set))
                allowed.push_back(cpu);
    }
#endif
    append(fmt::format("process may run on: {}", fmt::join(allowed, ",")));

    // One line per allowed core: design, cluster, capacity, frequency range and
    // measured speed relative to the fastest core.
    const auto names = CoreNames();
    const u32 iterations = options.quick ? 1'000'000 : 8'000'000;
    std::vector<double> times;
    for (const u32 cpu : allowed)
        times.push_back(TimeOnCpu(cpu, iterations, options.quick ? 1 : 3));
    double fastest = 0.0;
    for (const double t : times)
        if (t > 0.0 && (fastest == 0.0 || t < fastest))
            fastest = t;
    append(fmt::format("speed test: {} iterations per run, best of {}", iterations,
                       options.quick ? 1 : 3));
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        const std::string dir = fmt::format("/sys/devices/system/cpu/cpu{}/", allowed[i]);
        const auto name = names.find(allowed[i]);
        append(fmt::format(
            "cpu{}: {} cluster={} capacity={} khz={}..{} time_us={:.0f} relative={:.2f}",
            allowed[i], name != names.end() ? name->second : "unknown",
            ReadLine(dir + "topology/cluster_id"), ReadLine(dir + "cpu_capacity"),
            ReadLine(dir + "cpufreq/cpuinfo_min_freq"), ReadLine(dir + "cpufreq/cpuinfo_max_freq"),
            times[i], times[i] > 0.0 ? fastest / times[i] : 0.0));
    }
    return out;
}

} // namespace Claudhar::Probe
