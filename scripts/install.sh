#!/usr/bin/env bash
# Dev convenience: builds (if needed) and copies forgetful straight to a
# connected Move over SSH, for local iteration without going through
# schwung-manager. Not part of the release pipeline.
set -euo pipefail

cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
REMOTE_DIR="/data/UserData/schwung/modules/audio_fx/forgetful"

if [ ! -f dist/forgetful/forgetful.so ]; then
    echo "dist/forgetful/forgetful.so not found — building first..."
    ./scripts/build.sh
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
scp -q dist/forgetful/module.json  "$HOST:$REMOTE_DIR/.module.json.incoming"
ssh "$HOST" "cd '$REMOTE_DIR' && \
    chmod 755 .forgetful.so.incoming && \
    mv -f .forgetful.so.incoming forgetful.so && \
    mv -f .module.json.incoming module.json"

echo "Done. The running instance keeps the old code until you reload the"
echo "module (swap the slot away and back, or restart schwung)."
