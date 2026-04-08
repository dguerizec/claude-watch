#!/bin/bash
set -e
cd "$(dirname "$0")"

# Bundle TypeScript
npx esbuild src/app.ts --bundle --minify --outfile=dist/app.js --target=es2020

# Inline JS into HTML
mkdir -p dist
python3 - <<'PYEOF'
import re
html = open("src/index.html").read()
js = open("dist/app.js").read()
# Replace <script src="app.js"> with inline script
html = html.replace('<script src="app.js"></script>', f'<script>{js}</script>')
open("dist/index.html", "w").write(html)
print(f"Built dist/index.html ({len(html)} bytes)")
PYEOF

# Generate C header for ESP32
python3 ../components/wifi_manager/gen_html_header.py \
    dist/index.html \
    ../components/wifi_manager/settings_html.h \
    settings_html
echo "Generated settings_html.h"
