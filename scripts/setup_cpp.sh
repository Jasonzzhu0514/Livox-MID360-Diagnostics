#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

git submodule update --init --recursive external/Livox-SDK2 external/livox_ros_driver2
source scripts/prepare_config.sh
./scripts/check_offline.sh
./scripts/build_cpp_with_sdk2.sh

echo
echo "C++ CLI is ready:"
echo "  ./build/sdk2/livox_mid360_diagnostics"
echo "  ./build/sdk2/livox_mid360_diagnostics autoconfig"
echo "  ./build/sdk2/livox_mid360_diagnostics monitor"
echo "  ./build/sdk2/livox_mid360_diagnostics dump --duration 10 --points \"\$PWD/mid360_points.csv\" --imu \"\$PWD/mid360_imu.csv\""
