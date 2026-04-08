#!/usr/bin/env python3
"""Convert an HTML file into a C string header."""
import sys

src = sys.argv[1]
dst = sys.argv[2]
varname = sys.argv[3] if len(sys.argv) > 3 else "portal_html"

with open(src) as f:
    lines = f.read().split("\n")

with open(dst, "w") as f:
    f.write(f"/* Auto-generated — do not edit */\n")
    f.write(f"static const char {varname}[] =\n")
    for i, line in enumerate(lines):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        last = i == len(lines) - 1 and not escaped
        if not last:
            f.write(f'    "{escaped}\\n"\n')
    f.write(";\n")
