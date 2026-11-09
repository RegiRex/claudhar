// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Reads the process and device memory figures; see process_memory.h.

#include <cstdio>
#include <fmt/format.h>
#include <unistd.h>

#include "claudhar/diagnostics/process_memory.h"
#include "common/common_types.h"

namespace Claudhar::Diagnostics {

namespace {

/// The process's resident set in MiB from /proc/self/statm, whose second field counts
/// resident pages; 0 when it cannot be read.
u64 ResidentMiB() {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (!file) {
        return 0;
    }
    unsigned long long size = 0;
    unsigned long long resident = 0;
    const bool read = std::fscanf(file, "%llu %llu", &size, &resident) == 2;
    std::fclose(file);
    const long page = sysconf(_SC_PAGESIZE);
    return read && page > 0 ? (resident * static_cast<u64>(page)) >> 20 : 0;
}

/// The memory the device can still give out without swapping, in MiB, from the
/// MemAvailable line of /proc/meminfo; 0 when it cannot be read.
u64 AvailableMiB() {
    std::FILE* file = std::fopen("/proc/meminfo", "r");
    if (!file) {
        return 0;
    }
    char line[128];
    unsigned long long kib = 0;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
            break;
        }
    }
    std::fclose(file);
    return kib >> 10;
}

} // namespace

std::string ProcessMemoryReport() {
    const u64 resident = ResidentMiB();
    const u64 available = AvailableMiB();
    if (resident && available) {
        return fmt::format("process {} MiB resident, device {} MiB available", resident, available);
    }
    if (resident) {
        return fmt::format("process {} MiB resident", resident);
    }
    return available ? fmt::format("device {} MiB available", available) : std::string{};
}

} // namespace Claudhar::Diagnostics
