#!/usr/bin/env python3
"""Convert an HTML file into a C string header."""
import sys

src, dst = sys.argv[1], sys.argv[2]

with open(src) as f:
    lines = f.read().split("\n")

with open(dst, "w") as f:
    f.write("/* Auto-generated from portal.html — do not edit */\n")
    f.write("static const char portal_html[] =\n")
    for i, line in enumerate(lines):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        last = i == len(lines) - 1 and not escaped
        if not last:
            f.write(f'    "{escaped}\\n"\n')
    f.write(";\n")
