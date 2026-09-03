#!/usr/bin/env bash
# Dev convenience: builds (if needed) and copies forgetful straight to a
# connected Move over SSH, for local iteration without going through
# schwung-manager. Not part of the release pipeline.
set -euo pipefail

cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
REMOTE_DIR="/data/UserData/schwung/modules/audio_fx/forgetful"
BUILD_IMAGE="${FORGETFUL_BUILD_IMAGE:-forgetful-builder}"
RELOAD=1
[ "${1:-}" = "--no-reload" ] && RELOAD=0

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

# Deploy src/module.json as it stands — the version in the tree IS the
# version that lands on the Move.
#
# That only works because of the rebuild check above: this script used to
# copy dist/module.json, and dist/ was rebuilt only when the .so was
# MISSING, so a version bump alone left the old number in place. A Move
# ran the 0.4.0 code and reported "0.3.3" in Schwung Manager for a day.
#
# There was briefly a "0.0.0-dev+<version>-g<sha>" stamp here to make a
# hand-deploy unmistakable. It was correct and unreadable. Bump the patch
# version when you deploy a change instead — an ordinary version number
# people can say out loud.
#
# The one thing to keep in mind: a version deployed by hand and never
# tagged sits ABOVE the catalog, so Schwung Manager offers no update and
# the device quietly stays on an unreleased build. Tag what you deploy.
DEPLOY_JSON="dist/forgetful/.module.json.deploy"
cp src/module.json "$DEPLOY_JSON"

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
scp -q "$DEPLOY_JSON"              "$HOST:$REMOTE_DIR/.module.json.incoming"
scp -q src/help.json               "$HOST:$REMOTE_DIR/.help.json.incoming"
ssh "$HOST" "cd '$REMOTE_DIR' && \
    chmod 755 .forgetful.so.incoming && \
    mv -f .forgetful.so.incoming forgetful.so && \
    mv -f .module.json.incoming module.json && \
    mv -f .help.json.incoming help.json"

# ---- reload, then PROVE the running process picked it up ----------------
#
# Copying the file is not deploying it. The shim has the old .so dlopen'd
# for as long as the module sits in a slot, and the atomic rename gives the
# new file its own inode, so the process keeps executing the old one — from
# a file that no longer has a name.
#
# That is not theoretical: a Move sat on the previous build through FOUR
# deploys, reporting the new version in module.json (which is just a file)
# while running the old code, with 8 (deleted) regions in its maps. The
# advice printed here at the time — "swap the slot away and back" — did not
# take. So the script now reloads by default and, more importantly, REFUSES
# TO CLAIM SUCCESS until the mapped inode matches the one on disk.
#
# --no-reload skips it, for when you are mid-take and will restart later.
if [ "$RELOAD" = "0" ]; then
    echo
    echo "Deployed, NOT reloaded (--no-reload)."
    echo "The running instance keeps the old code until schwung restarts."
    exit 0
fi

echo "Reloading schwung so the new build is actually running..."
ROOT_HOST="root@${HOST#*@}"
if ! ssh -o ConnectTimeout=8 "$ROOT_HOST" true 2>/dev/null; then
    echo
    echo "Deployed, but could not reach $ROOT_HOST to restart schwung."
    echo "The running instance is still on the OLD code. Restart it by hand." >&2
    exit 1
fi

ssh "$ROOT_HOST" "/etc/init.d/move stop >/dev/null 2>&1 || true
for name in MoveOriginal Move MoveLauncher MoveMessageDisplay shadow_ui schwung link-subscriber display-server; do
  pids=\$(pidof \$name 2>/dev/null || true); [ -n \"\$pids\" ] && kill -9 \$pids 2>/dev/null || true
done
rm -f /dev/shm/move-shadow-* /dev/shm/move-display-*
pids=\$(fuser /dev/ablspi0.0 2>/dev/null || true); [ -n \"\$pids\" ] && kill -9 \$pids 2>/dev/null || true
sleep 3
/etc/init.d/move start >/dev/null 2>&1" || true

ssh "$ROOT_HOST" "
want=\$(stat -c %i '$REMOTE_DIR/forgetful.so')
i=0
while [ \$i -lt 30 ]; do
  pid=\$(pidof MoveOriginal 2>/dev/null | awk '{print \$1}')
  got=\$(grep forgetful /proc/\$pid/maps 2>/dev/null | awk '{print \$5}' | sort -u | sed -n 1p)
  if [ -n \"\$got\" ]; then
    if [ \"\$got\" = \"\$want\" ]; then
      echo \"  running the new build (inode \$got, 0 stale mappings: \$(grep -c '(deleted)' /proc/\$pid/maps))\"
      exit 0
    fi
    echo \"  STILL OLD: mapped inode \$got, on disk \$want\" >&2
    exit 1
  fi
  i=\$((i+1)); sleep 2
done
echo '  forgetful is not loaded in any slot — nothing to verify' >&2
exit 2"
rc=$?

echo
case $rc in
  0) echo "Done — deployed AND verified running." ;;
  2) echo "Deployed. Load Forgetful into a slot to run it." ;;
  *) echo "Deployed, but the running process did NOT pick it up." >&2; exit 1 ;;
esac
