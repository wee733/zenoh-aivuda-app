#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "[pre_install] Building zenoh_bridge_node in ${APP_ROOT}"

find_ros_setup() {
  for d in jazzy lyrical kilted humble; do
    if [[ -f "/opt/ros/${d}/setup.bash" ]]; then
      echo "/opt/ros/${d}/setup.bash"
      return 0
    fi
  done
  return 1
}

ROS_SETUP=""
if [[ -n "${ROS_DISTRO:-}" && -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]]; then
  ROS_SETUP="/opt/ros/${ROS_DISTRO}/setup.bash"
else
  ROS_SETUP="$(find_ros_setup || true)"
fi

if [[ -z "${ROS_SETUP}" ]]; then
  echo "[pre_install] ERROR: No ROS 2 installation found under /opt/ros/{jazzy,lyrical,kilted,humble}." >&2
  exit 1
fi

echo "[pre_install] Using ROS 2 setup: ${ROS_SETUP}"
# ROS setup scripts may reference unset variables under set -u
set +u
source "${ROS_SETUP}"
set -u

if ! command -v cmake >/dev/null 2>&1; then
  echo "[pre_install] ERROR: cmake not found. Please install cmake and build-essential." >&2
  exit 1
fi

BUILD_DIR="${APP_ROOT}/build"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cmake -S "${APP_ROOT}/src" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)"

if [[ ! -x "${BUILD_DIR}/zenoh_bridge_node" ]]; then
  echo "[pre_install] ERROR: Failed to build ${BUILD_DIR}/zenoh_bridge_node" >&2
  exit 1
fi

chmod +x "${APP_ROOT}/start.sh" "${APP_ROOT}/scripts/pre_install.sh"
echo "[pre_install] Built ${BUILD_DIR}/zenoh_bridge_node successfully (ROS_DISTRO=${ROS_DISTRO:-unknown})."
