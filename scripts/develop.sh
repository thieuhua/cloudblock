#!/usr/bin/env bash
# Build and test CloudBlock in the project directory; no sudo/system changes.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
echo "CloudBlock local-only: build/test complete."
echo "Example: ./build/cloudblock ./store status"
