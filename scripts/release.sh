#!/usr/bin/env bash
# The ONLY thing in this repo that reaches Schwung Manager.
#
# Two tracks, deliberately separate:
#
#   scripts/install.sh   local dev build -> your Move, over SSH.
#                        Touches nothing else. No catalog, no release, no
#                        GitHub. Run it as often as you like.
#
#   scripts/release.sh   tag -> GitHub Actions -> release.json on main ->
#                        the catalog -> everyone's Module Store.
#                        Run it when you decide to ship, never as a
#                        side effect of anything else.
#
# The Move can sit ahead of the catalog for as long as you want. A
# hand-deployed version is NEWER than the released one, so Schwung Manager
# offers no update and does not nag.
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="$(python3 -c "import json;print(json.load(open('src/module.json'))['version'])")"
TAG="v$VERSION"

echo "Releasing $TAG"

# ---- refuse to ship something that is not what is in the tree ----
if [ -n "$(git status --porcelain)" ]; then
    echo "Working tree is dirty. Commit or stash first — the tag has to name" >&2
    echo "a commit that actually contains what you are shipping." >&2
    git status --short >&2
    exit 1
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
    echo "Tag $TAG already exists. Bump 'version' in src/module.json first." >&2
    exit 1
fi

# The PREVIOUS release's workflow commits release.json to main, so the
# remote is routinely one commit ahead of a local tree that has not fetched
# since. Left to git, that surfaces as a rejected push and a wall of hint:
# text AFTER the suite has already run. Rebase onto it first, then re-run
# the suite against what will actually be tagged — a rebase can conflict or
# change behaviour, and tagging something the tests never saw is the whole
# thing this script exists to prevent.
echo "Fetching origin..."
git fetch origin --quiet
if [ -n "$(git log --oneline HEAD..origin/main 2>/dev/null)" ]; then
    echo "  origin/main is ahead — rebasing onto it:"
    git log --oneline HEAD..origin/main | sed 's/^/    /'
    git rebase origin/main || {
        echo "Rebase failed. Resolve it, then run this again." >&2
        exit 1
    }
fi

echo "Running the test suite..."
bash tests/run.sh >/dev/null || { echo "Tests failed — not releasing." >&2; exit 1; }
echo "  suite passed"

# ---- BRANCH FIRST, THEN TAG ----
#
# Not interchangeable. Pushing the tag first once produced a release built
# from a commit that was not on main, and the release workflow then
# committed release.json for that version onto a main whose source was a
# version behind. Push the branch, let it land, and only then tag it.
echo "Pushing main..."
git push origin main

echo "Tagging $TAG..."
git tag -a "$TAG" -m "$TAG"
git push origin "$TAG"

echo
echo "Done. GitHub Actions is building $TAG now; it will attach the tarball"
echo "and update release.json on main. Schwung Manager picks that up within"
echo "a few minutes, and only then does anyone else see the update."
