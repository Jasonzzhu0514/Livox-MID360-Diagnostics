#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 \"\$LIVOX_ROS_DRIVER2_WS\" [rviz_MID360_launch.py]" >&2
  echo "The submodule at external/livox_ros_driver2 is source code; pass a built ROS2 workspace." >&2
  exit 2
fi

ws="$1"
launch_file="${2:-rviz_MID360_launch.py}"

source "$ws/install/setup.bash"
ros2 launch livox_ros_driver2 "$launch_file"
