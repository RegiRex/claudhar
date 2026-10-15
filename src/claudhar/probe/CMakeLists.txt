# Copyright 2026 Claudhar Project
# Licensed under GPLv3 or any later version
# Refer to the LICENSE.txt file included.

# Phase 0 device probe: a library used by the Android probe screen, plus a
# command-line tool for Linux that CI runs on a CPU Vulkan driver (lavapipe).
add_library(claudhar_probe STATIC EXCLUDE_FROM_ALL
    probe.h
    probe_cpu.cpp
    probe_vulkan.cpp
)
create_target_directory_groups(claudhar_probe)
target_link_libraries(claudhar_probe PUBLIC citra_common PRIVATE video_core vulkan-headers)

if (NOT ANDROID)
    add_executable(claudhar-probe probe_main.cpp)
    target_link_libraries(claudhar-probe PRIVATE claudhar_probe)
    if (ENABLE_TESTS)
        # Fails when the probe reports an error or the two shaders disagree.
        add_test(NAME claudhar_probe COMMAND claudhar-probe --quick --check)
    endif()
endif()
