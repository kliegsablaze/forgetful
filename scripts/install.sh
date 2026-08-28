#!/usr/bin/env bash
# Dev convenience: builds (if needed) and copies forgetful straight to a
# connected Move over SSH, for local iteration without going through
# schwung-manager. Not part of the release pipeline.
set -euo pipefail

cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
REMOTE_DIR="/data/UserData/schwung/modules/audio_fx/forgetful"
BUILD_IMAGE="${FORGETFUL_BUILD_IMAGE:-forgetful-builder}"

# Rebuild when dist/ is behind the tree, not only when it is missing.
#
# "Build only if absent" is how a stale artifact gets deployed: the module
# was hand-installed while dist/ still held an older build, so the Move ran
# code from one commit and metadata from another. Anything under src/
# counts, module.json included — build.sh copies it into dist/, so a
# version bump alone leaves dist/module.json behind.
# build.sh needs the aarch64 cross toolchain, which lives in the image
# scripts/Dockerfile builds. Go through Docker unless the compiler is
# already on PATH (an aarch64 host, or running inside the container).
build() {
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        ./scripts/build.sh
    elif command -v docker >/dev/null 2>&1; then
        docker image inspect "$BUILD_IMAGE" >/dev/null 2>&1 || {
            echo "Building the $BUILD_IMAGE image (first run only)..."
            docker build -q -t "$BUILD_IMAGE" -f scripts/Dockerfile . >/dev/null
        }
        docker run --rm -v "$PWD:/build" -w /build "$BUILD_IMAGE" ./scripts/build.sh
    else
        echo "Need either the aarch64 cross toolchain or Docker to build." >&2
        exit 1
    fi
}

if [ ! -f dist/forgetful/forgetful.so ]; then
    echo "dist/forgetful/forgetful.so not found — building first..."
    build
elif [ -n "$(find src -type f -newer dist/forgetful/forgetful.so 2>/dev/null | head -n 1)" ]; then
    echo "dist/ is older than src/ — rebuilding so the deploy matches the tree..."
    find src -type f -newer dist/forgetful/forgetful.so | sed 's/^/    newer: /'
    build
fi

# Refuse to ship a host build to the Move. Getting this wrong is silent
# until the module fails to load: scripts/build.sh falls back to a native
# build when CROSS_PREFIX is empty, and dist/ then holds a binary for the
# wrong architecture entirely.
if ! head -c 20 dist/forgetful/forgetful.so | od -An -tx1 |
     tr -d ' \n' | grep -Eq '^7f454c46.{28}b700'; then
    echo "dist/forgetful/forgetful.so is not an aarch64 ELF — build it with" >&2
    echo "Docker (scripts/Dockerfile) rather than a native build." >&2
    exit 1
fi

# Stamp the deployed module.json so a hand-installed build can never be
# mistaken for a release in Schwung Manager.
#
# This script ships whatever version string happens to be in the tree, and
# that is routinely mid-flight: the v0.4.0 bump landed in a later commit
# than the last hand-deploy, so a Move running the 0.4.0 code reported
# "0.3.3" in the Manager for a day. The Manager was right — it reads the
# installed module.json and has no way to know the .so is newer than its
# own metadata claims.
#
# The base MUST be 0.0.0 rather than the real version with a -dev suffix.
# schwung-manager's isNewerSemver() splits on "." and Atoi()s each part
# IGNORING the error, so "0.4.0-dev+gabc1234" parses as 0.4.0 and the
# Manager would report the build as up to date forever — strictly worse
# than the stale number this is fixing. Verified against a verbatim copy
# of that function: 0.4.0-dev+g... -> no update offered, 0.0.0-dev+... ->
# update offered against 0.4.0, 0.4.1 and 1.0.0 alike.
#
# Set FORGETFUL_DEV_STAMP=0 to deploy the raw version instead.
STAMPED_JSON="dist/forgetful/.module.json.stamped"
if [ "${FORGETFUL_DEV_STAMP:-1}" = "1" ]; then
    sha="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
    dirty=""
    [ -n "$(git status --porcelain 2>/dev/null)" ] && dirty="-dirty"
    python3 - "$STAMPED_JSON" "$sha" "$dirty" <<'PYSTAMP'
import json, sys
out, sha, dirty = sys.argv[1], sys.argv[2], sys.argv[3]
m = json.load(open("src/module.json"))
m["version"] = "0.0.0-dev+%s-g%s%s" % (m["version"], sha, dirty)
# ensure_ascii=False: the catalog lesson — escaping non-ASCII rewrites
# characters nobody asked to touch.
json.dump(m, open(out, "w"), indent=2, ensure_ascii=False)
open(out, "a").write("\n")
print("  stamping deployed version as %s" % m["version"])
PYSTAMP
else
    cp src/module.json "$STAMPED_JSON"
    echo "  FORGETFUL_DEV_STAMP=0 — deploying the raw version string"
fi

echo "Deploying to $HOST:$REMOTE_DIR ..."
ssh "$HOST" "mkdir -p '$REMOTE_DIR'"

# Upload beside the target, then rename over it.
#
# NEVER scp straight onto the live path. scp opens the destination with
# O_TRUNC and rewrites it in place, and the shim has this .so dlopen()'d
# whenever the module sits in a chain slot — so the file backing a live
# mapping is truncated under it and the next page fault executes garbage.
# That is a hard SIGSEGV in the shim, taking Move's whole audio process
# down with it; it took three of them, one per upload, to spot the
# pattern (debug.log, 2026-08-27).
#
# rename(2) is atomic and gives the new file its own inode, so a running
# instance keeps its old mapping intact and simply carries on with the old
# code until the module is next loaded. Temp file goes in the SAME
# directory, or the rename becomes a copy and the guarantee is lost.
scp -q dist/forgetful/forgetful.so "$HOST:$REMOTE_DIR/.forgetful.so.incoming"
scp -q "$STAMPED_JSON"             "$HOST:$REMOTE_DIR/.module.json.incoming"
ssh "$HOST" "cd '$REMOTE_DIR' && \
    chmod 755 .forgetful.so.incoming && \
    mv -f .forgetful.so.incoming forgetful.so && \
    mv -f .module.json.incoming module.json"

echo "Done. The running instance keeps the old code until you reload the"
echo "module (swap the slot away and back, or restart schwung)."
echo "This is a DEV build: Schwung Manager will show it as outdated and"
echo "offer the catalog release, which is correct — install from there to"
echo "get back to a real version."
