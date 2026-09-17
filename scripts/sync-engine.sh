#!/bin/sh
# sync-engine.sh — put the engine pinned by GEIST_REF at GEIST_DIR.
#
# Verifies on every run rather than only cloning when missing. The old
# clone-if-missing check reported "libgeist already present" for whatever was
# checked out, so a GEIST_REF bump silently built against the previous engine —
# the local-artefact-read-as-truth failure docs/MODEL-ADAPTATION.md describes.
# Same contract as geist-diktat's scripts/sync-engine.sh.
#
# GEIST_FETCH=1 fetches origin first, for refs that move (a branch).
set -eu

: "${GEIST_REPO:?}" "${GEIST_REF:?}" "${GEIST_DIR:?}"

# An empty directory is what a checkout made while deps/geist was still a
# gitlink leaves behind; rmdir refuses anything that holds files.
if [ -d "$GEIST_DIR" ] && [ ! -d "$GEIST_DIR/.git" ]; then
    rmdir "$GEIST_DIR" 2>/dev/null || true
fi

# Never rm -rf a directory we did not create: it may hold models or local work.
if [ ! -e "$GEIST_DIR" ]; then
    echo "engine: cloning $GEIST_REPO -> $GEIST_DIR"
    git clone --quiet "$GEIST_REPO" "$GEIST_DIR"
elif [ ! -d "$GEIST_DIR/.git" ]; then
    echo "engine: $GEIST_DIR exists but is not a git checkout." >&2
    echo "engine: move it aside or run 'make distclean', then build again." >&2
    exit 1
fi

if [ "${GEIST_FETCH:-0}" = 1 ]; then
    git -C "$GEIST_DIR" fetch --quiet --tags origin
fi

# Resolve the pin locally; only reach the network when the ref is unknown.
want=$(git -C "$GEIST_DIR" rev-parse --quiet --verify "$GEIST_REF^{commit}" || true)
if [ -z "$want" ]; then
    echo "engine: fetching $GEIST_REF"
    git -C "$GEIST_DIR" fetch --quiet --tags origin
    want=$(git -C "$GEIST_DIR" rev-parse --verify "$GEIST_REF^{commit}")
fi

if [ "$(git -C "$GEIST_DIR" rev-parse HEAD)" != "$want" ]; then
    echo "engine: $GEIST_REF -> $(echo "$want" | cut -c1-12)"
    git -C "$GEIST_DIR" checkout --quiet --detach "$want"
fi
