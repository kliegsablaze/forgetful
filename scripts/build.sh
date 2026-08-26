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

cd dist
tar -czvf forgetful-module.tar.gz forgetful/
cd ..

echo ""
echo "Tarball: dist/forgetful-module.tar.gz"
