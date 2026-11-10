#!/usr/bin/env bash
# Zeste Linux build (GCC or Clang + Ninja), with the same options as the release build.
# Usage: tools/zeste/build_linux.sh [configure] [nocheck]
# After the build, tools/zeste/check.sh runs the unit tests and the test ROM (nocheck skips it).
set -eu
cd "$(dirname "$0")/../.."
check=1
for a in "$@"; do
    case "$a" in
        configure)
            cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DENABLE_CCACHE=ON
            ;;
        nocheck) check=0 ;;
        *) echo "usage: $0 [configure] [nocheck]" >&2; exit 1 ;;
    esac
done
cmake --build build
if [ $check -eq 1 ]; then
    tools/zeste/check.sh
fi
