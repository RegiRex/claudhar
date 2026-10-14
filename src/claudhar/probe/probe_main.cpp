// Copyright 2026 Claudhar Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// claudhar-probe: prints the device probe report on Linux. "--quick" measures less,
// for CI on a CPU Vulkan driver. "--check" exits with an error when the Vulkan part
// failed or the ubershader and specialized shader drew different images, so CI
// catches a broken probe before it reaches a phone.

#include <cstdio>
#include <string_view>
#include "claudhar/probe/probe.h"

int main(int argc, char** argv) {
    Claudhar::Probe::Options options;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        options.quick |= std::string_view{argv[i]} == "--quick";
        check |= std::string_view{argv[i]} == "--check";
    }
    const std::string vulkan = Claudhar::Probe::ReportVulkan(options);
    std::fputs((Claudhar::Probe::ReportCpu(options) + vulkan).c_str(), stdout);
    const bool failed = vulkan.find("error:") != std::string::npos ||
                        vulkan.find("MISMATCH") != std::string::npos ||
                        vulkan.find("ubershader /") == std::string::npos;
    return check && failed ? 1 : 0;
}
