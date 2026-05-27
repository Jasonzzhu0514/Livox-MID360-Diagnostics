#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

git submodule update --init --recursive external/Livox-SDK2 external/livox_ros_driver2
source scripts/prepare_config.sh
python3 -m pip install -e .

echo
echo "Python CLI is ready:"
echo "  livox-mid360-diagnostics autoconfig"
echo "  livox-mid360-diagnostics udp-monitor"
