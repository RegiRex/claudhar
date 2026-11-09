# Copyright 2026 Claudhar Project
# Licensed under GPLv3 or any later version
# Refer to the LICENSE.txt file included.

# Figures the logs report about the emulation thread and the process: where frame time goes
# and how much memory is in use. Compiled into video_core, whose renderer logs them.
target_sources(video_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/process_memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/process_memory.h
)
