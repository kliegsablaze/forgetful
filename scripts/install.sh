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

echo "Deploying to $HOST:$REMOTE_DIR ..."
ssh "$HOST" "mkdir -p $REMOTE_DIR"
scp dist/forgetful/forgetful.so dist/forgetful/module.json "$HOST:$REMOTE_DIR/"

echo "Done. Reload the module (or restart schwung) on the Move to pick it up."
