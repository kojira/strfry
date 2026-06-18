#!/usr/bin/env bash
#
# Build strfry natively on macOS (no Docker).
#
# Why: running strfry in Docker Desktop for Mac forces the LMDB database through the
# VM file-sharing layer (virtiofs) and caps the page cache at the VM's RAM. A native
# build mmaps the DB directly and uses all of the host's RAM as cache, which is much
# faster for queries (especially cold scans on a large DB).
#
# Prereqs (Homebrew):
#   brew install flatbuffers lmdb libuv openssl@3 zstd secp256k1
# Perl codegen modules (Regexp::Grammars, Template) are vendored under golpe/vendor.
#
# Usage:
#   ./build-native-macos.sh
#
set -euo pipefail

if [ "$(uname)" != "Darwin" ]; then
    echo "This script is for macOS. On Linux just run 'make'." >&2
    exit 1
fi

BREW_PREFIX="$(brew --prefix)"

# Apply the macOS (kqueue) port of hoytech's file_change_monitor into the submodule.
# The upstream version is Linux-only (inotify); this drop-in keeps Linux unchanged via #ifdef.
FCM="golpe/external/hoytech-cpp/hoytech/file_change_monitor.h"
if [ -f patches/file_change_monitor.h ] && [ -f "$FCM" ]; then
    if ! cmp -s patches/file_change_monitor.h "$FCM"; then
        echo "Applying macOS kqueue file_change_monitor patch into submodule..."
        cp patches/file_change_monitor.h "$FCM"
    fi
fi

# Homebrew openssl@3 is keg-only; expose all needed headers/libs to every compile step
# (uWebSockets, golpe codegen, strfry) without editing any Makefile.
export PATH="$BREW_PREFIX/bin:$PATH"
export CPATH="$BREW_PREFIX/include:$BREW_PREFIX/opt/openssl@3/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="$BREW_PREFIX/lib:$BREW_PREFIX/opt/openssl@3/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"

mkdir -p build
make -j"$(sysctl -n hw.ncpu)" "$@"

echo
echo "Built native strfry. Run it with:"
echo "  DYLD_LIBRARY_PATH=$BREW_PREFIX/lib ./strfry --config <your.conf> relay"
