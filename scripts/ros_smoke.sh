#!/usr/bin/env bash
#
# SPDX-License-Identifier: MIT
#
# End-to-end smoke test for the ROS 2 layer. Expects:
#   - erde core installed somewhere on CMAKE_PREFIX_PATH (or in this checkout's build/)
#   - the ros2 CLI sourced (/opt/ros/<distro>/setup.bash)
#   - erde_ros built into ./install (colcon build --base-paths erde_ros)
#
# It generates its own clip, runs the node, and fails if no CompressedImage arrives.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$here"

if [ -z "${ROS_DISTRO:-}" ]; then
  echo "ROS_DISTRO is not set - source /opt/ros/<distro>/setup.bash first" >&2
  exit 2
fi
if [ ! -f install/setup.bash ]; then
  echo "install/setup.bash missing - run: colcon build --base-paths erde_ros" >&2
  exit 2
fi

source "/opt/ros/${ROS_DISTRO}/setup.bash"
source install/setup.bash

clip=/tmp/erde-ros-smoke.mp4
./build/erde gen "$clip" --size 320x240 --frames 300 --fps 30 --crf 23 >/dev/null 2>&1

log=/tmp/erde-ros-smoke.log
: > "$log"
setsid ros2 run erde_ros erde_node --ros-args \
  -p "input:=$clip" -p fps:=30 -p codec:=libx264 > "$log" 2>&1 &
node_pid=$!
trap 'kill -INT -"$node_pid" 2>/dev/null || true; pkill -f erde_node 2>/dev/null || true' EXIT

sleep 6

state=$(timeout 10 ros2 lifecycle get /erde 2>/dev/null | tr -d '\r')
if [[ "$state" != active* ]]; then
  echo "node is not active: '$state'" >&2
  tail -20 "$log" >&2
  exit 1
fi

format=$(timeout 25 ros2 topic echo --once /erde/image/compressed --field format 2>/dev/null | head -1 | tr -d '\r')
if [ -z "$format" ]; then
  echo "no CompressedImage arrived on /erde/image/compressed" >&2
  tail -20 "$log" >&2
  exit 1
fi

stats=$(timeout 10 ros2 service call /erde/stats std_srvs/srv/Trigger 2>/dev/null | tail -1)
echo "lifecycle: $state"
echo "message format: $format"
echo "stats: $stats"

case "$stats" in
  *"published="*) ;;
  *) echo "stats service did not report counters" >&2; exit 1 ;;
esac

echo "ROS 2 smoke test passed"
