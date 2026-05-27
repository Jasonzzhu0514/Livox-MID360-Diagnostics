#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
default_source="$repo_root/external/Livox-SDK2/samples/livox_lidar_quick_start/mid360_config.json"
fallback_source="$repo_root/external/livox_ros_driver2/config/MID360_config.json"
config_path="${1:-${LIVOX_MID360_CONFIG:-$repo_root/config/MID360_config.local.json}}"

if [[ ! -f "$default_source" ]]; then
  default_source="$fallback_source"
fi

if [[ ! -f "$default_source" ]]; then
  echo "MID360 config template not found." >&2
  echo "Run: git submodule update --init --recursive external/Livox-SDK2 external/livox_ros_driver2" >&2
  return 2 2>/dev/null || exit 2
fi

mkdir -p "$(dirname "$config_path")"
if [[ -f "$config_path" ]]; then
  echo "config exists: $config_path"
else
  cp "$default_source" "$config_path"
  echo "created config: $config_path"
fi

export LIVOX_MID360_DIAGNOSTICS_ROOT="$repo_root"
export LIVOX_MID360_CONFIG="$config_path"
export LIVOX_SDK2_ROOT="$repo_root/external/Livox-SDK2"

echo "LIVOX_MID360_DIAGNOSTICS_ROOT=$LIVOX_MID360_DIAGNOSTICS_ROOT"
echo "LIVOX_MID360_CONFIG=$LIVOX_MID360_CONFIG"
echo "LIVOX_SDK2_ROOT=$LIVOX_SDK2_ROOT"
echo
echo "To keep these variables in your current shell, run:"
echo "source scripts/prepare_config.sh"
