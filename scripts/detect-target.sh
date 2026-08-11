#!/bin/sh
# Which engine target this host builds. Output: mac | mac-omp | pi5 | linux | unknown
#
# A script rather than a $(shell ...) one-liner because the natural spelling is
# a `case`, and every `)` in it would close make's own $(...) — the kind of
# quoting puzzle that gets solved once and misread forever.
#
# This deliberately does NOT ask deps/geist/mk/detect-target.sh, even though it
# mirrors it. That file does not exist on a clean checkout, and the old fallback
# said `mac` when it was missing — so CI built the macOS target on Linux and
# died inside libgeist with an unrelated error (#105). geistshell must be able
# to name its own target before it has cloned anything.
#
# Override at make-time: make GEIST_TARGET=linux
set -eu

case "$(uname -s)" in
    Darwin)
        libomp_prefix="${LIBOMP_PREFIX:-/opt/homebrew/opt/libomp}"
        if [ -f "$libomp_prefix/lib/libomp.dylib" ]; then
            echo "mac-omp"
        else
            echo "mac"
        fi
        ;;
    Linux)
        case "$(uname -m)" in
            aarch64|arm64) echo "pi5" ;;
            *)             echo "linux" ;;
        esac
        ;;
    *)
        echo "unknown"
        ;;
esac
