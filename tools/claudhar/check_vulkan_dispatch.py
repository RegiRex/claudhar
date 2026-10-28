#!/usr/bin/env python3
# Copyright 2026 Claudhar Project
# Licensed under GPLv3 or any later version
# Refer to the LICENSE.txt file included.
"""Fails when vulkan-hpp's DispatchLoaderDynamic::init is compiled in more than one place.

init() is an inline function whose body depends on the VK_USE_PLATFORM_* macros of the
file that calls it. Two files with different macros produce two different definitions,
and the linker keeps only one of them for every caller. In Claudhar this once dropped the
Android surface function from the renderer's dispatcher and crashed every game. Each init
overload must therefore be defined in exactly one object; code outside Azahar's Vulkan
setup files uses Vulkan::InitDispatcher. Usage: check_vulkan_dispatch.py <build dir>."""
import pathlib
import subprocess
import sys

objects = {}
for path in pathlib.Path(sys.argv[1]).rglob("*.o"):
    symbols = subprocess.run(["nm", "-C", "--defined-only", str(path)], capture_output=True,
                             text=True).stdout
    for line in symbols.splitlines():
        name = line.split(" ", 2)[-1]
        if "DispatchLoaderDynamic::init(" in name:
            objects.setdefault(name, []).append(str(path))
problems = {name: paths for name, paths in objects.items() if len(paths) > 1}
for name, paths in problems.items():
    print(f"{name} is defined in {len(paths)} objects:\n  " + "\n  ".join(paths))
print("Vulkan dispatcher init: one definition each" if not problems else "")
sys.exit(1 if problems else 0)
