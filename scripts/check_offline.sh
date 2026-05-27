#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

python3 -m compileall -q src/python

if [[ -f build/offline/CMakeCache.txt ]] && ! grep -Fq "$repo_root" build/offline/CMakeCache.txt; then
  rm -rf build/offline
fi

cmake -S . -B build/offline -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/offline --target mid360_config_tool livox_mid360_diagnostics -j"$(nproc)"

echo "offline checks passed"
