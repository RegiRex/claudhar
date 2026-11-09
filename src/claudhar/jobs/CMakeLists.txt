# Copyright 2026 Claudhar Project
# Licensed under GPLv3 or any later version
# Refer to the LICENSE.txt file included.

# Running work on several cores: the parallel loop that shades CPU-vertex draws, the start
# of the job system Phase 3 plans. Compiled into video_core, its user.
target_sources(video_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/parallel_for.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parallel_for.h
)
