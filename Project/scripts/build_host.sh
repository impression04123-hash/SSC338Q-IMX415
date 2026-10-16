#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:-"$root/build/host"}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build" --parallel "${JOBS:-2}"
ctest --test-dir "$build" --output-on-failure
