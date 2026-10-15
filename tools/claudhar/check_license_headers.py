#!/usr/bin/env python3
# Copyright 2026 Claudhar Project
# Licensed under GPLv3 or any later version
# Refer to the LICENSE.txt file included.
"""Checks Claudhar's copyleft notices on the C++ and Kotlin files changed since the
Azahar import. A new file must start with the Claudhar GPLv3-or-later header. A
modified Azahar file keeps its own header and must carry the Claudhar change notice
near the top, as GPLv3 section 5(a) asks of modified files. Run from the repository
root; exits 1 and lists the offending files."""
import re
import subprocess
import sys

IMPORT = "98c8eac"  # "Import Azahar 2126.2 as the Claudhar base".
HEADER = re.compile(r"\A// Copyright \d{4}(-\d{4})? Claudhar Project\n"
                    r"// Licensed under GPLv3 or any later version\n"
                    r"// Refer to the LICENSE.txt file included.\n")
NOTICE = "Modified by the Claudhar Project"
EXTENSIONS = (".cpp", ".h", ".kt", ".kts")


def git(*args):
    """Runs git and returns its output lines."""
    result = subprocess.run(["git", *args], check=True, capture_output=True, text=True)
    return result.stdout.splitlines()


azahar = set(git("ls-tree", "-r", "--name-only", IMPORT))
changed = [name for name in git("diff", "--name-only", "--diff-filter=AM", IMPORT, "HEAD")
           if name.endswith(EXTENSIONS)]
problems = []
for name in changed:
    with open(name, encoding="utf-8") as file:
        text = file.read()
    if name in azahar:
        if NOTICE not in "".join(text.splitlines(keepends=True)[:6]):
            problems.append(f"{name}: modified Azahar file without the Claudhar change notice")
    elif not HEADER.match(text):
        problems.append(f"{name}: new file without the Claudhar GPL header")
print("\n".join(problems) or f"license notices OK ({len(changed)} changed files)")
sys.exit(1 if problems else 0)
