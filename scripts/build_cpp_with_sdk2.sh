#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
  echo "usage: $0 [\"\$LIVOX_SDK2_ROOT\"]" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${1:-${LIVOX_SDK2_ROOT:-$repo_root/external/Livox-SDK2}}"

if [[ ! -f "$sdk_root/include/livox_lidar_api.h" ]]; then
  echo "Livox-SDK2 not found at: $sdk_root" >&2
  echo "Run: git submodule update --init --recursive external/Livox-SDK2" >&2
  exit 2
fi

if [[ ! -f "$sdk_root/build/sdk_core/liblivox_lidar_sdk_static.a" && ! -f "$sdk_root/build/sdk_core/liblivox_lidar_sdk_shared.so" ]]; then
  if [[ -f "$sdk_root/build/CMakeCache.txt" ]] && ! grep -Fq "$sdk_root" "$sdk_root/build/CMakeCache.txt"; then
    rm -rf "$sdk_root/build"
  fi
  cmake -S "$sdk_root" -B "$sdk_root/build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS:-} -include cstdint"
  cmake --build "$sdk_root/build" --target livox_lidar_sdk_static -j"$(nproc)"
fi

if [[ -f "$repo_root/build/sdk2/CMakeCache.txt" ]] && ! grep -Fq "$repo_root" "$repo_root/build/sdk2/CMakeCache.txt"; then
  rm -rf "$repo_root/build/sdk2"
fi

cmake -S "$repo_root" -B "$repo_root/build/sdk2" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLIVOX_SDK2_ROOT="$sdk_root"
cmake --build "$repo_root/build/sdk2" -j"$(nproc)"
