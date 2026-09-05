#!/usr/bin/env bash
# Builds forgetful.so for aarch64 (Ableton Move) and packages dist/forgetful-module.tar.gz.
#
# Set CROSS_PREFIX to override the toolchain prefix (defaults to
# aarch64-linux-gnu-, matching the Docker cross-compilation environment in
# scripts/Dockerfile). Run natively on an aarch64 host by setting
# CROSS_PREFIX= (empty).
set -euo pipefail

cd "$(dirname "$0")/.."

CROSS_PREFIX="${CROSS_PREFIX-aarch64-linux-gnu-}"

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Error: missing compiler '${CROSS_PREFIX}gcc'" >&2
    echo "Run inside scripts/Dockerfile's container, or set CROSS_PREFIX=" \
         "for a native build." >&2
    exit 1
fi

echo "=== Building forgetful (target: ${CROSS_PREFIX:-native}) ==="

rm -rf dist
mkdir -p dist/forgetful

"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/dsp/forgetful.c \
    -o dist/forgetful/forgetful.so \
    -Isrc/dsp \
    -lm

cp src/module.json dist/forgetful/module.json
# On-device help, shown under Help -> Modules in the shadow UI. The host
# discovers it by scanning modules/<category>/<id>/ at runtime — no
# module.json entry — so it only has to land in the tarball.
cp src/help.json dist/forgetful/help.json
# The module's own drawing code (Schwung 1.2). canvas.js is loaded by the
# shadow UI when the component's chain_params declare a "custom:" viz kind
# and registers the in-grid widget; cards.js is loaded on the first touch
# of a knob declaring card_script. Both are discovered by path inside the
# module directory — canvas.js by name, cards.js by the card_script value
# — so, like help.json, they only have to land in the tarball.
#
# A missing file here is SILENT: the host logs it and draws the built-in
# widget, which is also what an older Schwung and a one-strike disable
# look like. Nothing on screen would tell you the copy was forgotten.
cp src/canvas.js dist/forgetful/canvas.js
cp src/cards.js  dist/forgetful/cards.js

cd dist
tar -czvf forgetful-module.tar.gz forgetful/
cd ..

echo ""
echo "Tarball: dist/forgetful-module.tar.gz"
