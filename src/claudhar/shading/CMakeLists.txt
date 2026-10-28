# Copyright 2026 Claudhar Project
# Licensed under GPLv3 or any later version
# Refer to the LICENSE.txt file included.

# Phase 1 zero-pause shading. The fragment ubershader and the draw counters are compiled
# into video_core, whose Vulkan renderer uses them. claudhar-ubershader-check compares it pixel for pixel
# with Azahar's specialized shaders on a Vulkan device (lavapipe in CI).
target_sources(video_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/draw_ledger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/draw_ledger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_ubershader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_ubershader.h
)

if (NOT ANDROID)
    add_executable(claudhar-ubershader-check ubershader_check.cpp)
    target_link_libraries(claudhar-ubershader-check PRIVATE video_core citra_common vulkan-headers sirit)
    if (ENABLE_TESTS)
        # Fails when the ubershader and a specialized shader disagree on any test.
        add_test(NAME claudhar_ubershader COMMAND claudhar-ubershader-check --count 300)
    endif()
endif()
