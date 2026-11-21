#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_PATH="${AIVUDA_APP_CONFIG_PATH:-${APP_ROOT}/config/default_config.yaml}"
RUNTIME_DIR="${AIVUDA_APP_RUNTIME_DATA_PATH:-${APP_ROOT}/runtime}"
mkdir -p "${RUNTIME_DIR}"
RUNTIME_DIR="$(cd "${RUNTIME_DIR}" && pwd -P)"

echo "[start.sh] zenoh bridge starting: app=${AIVUDA_APP_ID:-zenoh}@${AIVUDA_APP_VERSION:-dev}"
echo "[start.sh] config=${CONFIG_PATH}"
echo "[start.sh] runtime=${RUNTIME_DIR}"

# Parse AivudaOS sys/ros YAML config + persisted Web UI bridge_settings.json
ENV_FILE="$(mktemp "${RUNTIME_DIR}/.runtime_env.XXXXXXXX")"
trap 'rm -f -- "${ENV_FILE}"' EXIT
RUNTIME_JSON="${RUNTIME_DIR}/runtime_config.json"

python3 - "${CONFIG_PATH}" "${RUNTIME_DIR}" "${RUNTIME_JSON}" "${ENV_FILE}" <<'PY'
import json
import os
import stat
import sys
import tempfile
from shlex import quote
from pathlib import Path
import yaml

cfg_path, runtime_dir, runtime_json_path, env_file_path = sys.argv[1:]
cfg = yaml.safe_load(Path(cfg_path).read_text(encoding="utf-8")) or {}

sys_cfg = cfg.get("sys", {}) or {}
role_cfg = sys_cfg.get("role", {}) or {}
ros_cfg = cfg.get("ros", {}) or {}

role_id = role_cfg.get("id", 1)
robot_type = str(sys_cfg.get("robot_type", "fastlio2_rotor") or "fastlio2_rotor")
default_agent_id = f"uav{role_id}"

# Only the namespace remap needs the UI identity here. Do not copy policy
# into runtime defaults: the binary reads one authoritative snapshot under
# its instance lock and enforces it before rclcpp::init (also for direct exec).
bridge_settings_path = Path(runtime_dir) / "bridge_settings.json"
try:
    st = bridge_settings_path.lstat()
except FileNotFoundError:
    bridge_settings = {}
else:
    if not stat.S_ISREG(st.st_mode):
        raise ValueError(f"not a regular settings file: {bridge_settings_path}")
    bridge_settings = json.loads(bridge_settings_path.read_text(encoding="utf-8"))
    if not isinstance(bridge_settings, dict):
        raise ValueError(f"expected object: {bridge_settings_path}")
custom_agent_id = str(bridge_settings.get("agent_id", "") or "").strip().strip("/")
agent_id = custom_agent_id if custom_agent_id else default_agent_id

ros_distro = str(ros_cfg.get("distro", "auto") or "auto").strip()
ros_setup_script = str(ros_cfg.get("setup_script", "") or "").strip()
discovery_range = str(ros_cfg.get("discovery_range", "LOCALHOST") or "LOCALHOST").strip()
zenoh_cfg = cfg.get("zenoh", {}) or {}
if isinstance(zenoh_cfg, dict) and "ros_domain_id" in zenoh_cfg:
    domain_id = int(zenoh_cfg.get("ros_domain_id", -1))
elif "domain_id" in ros_cfg:
    domain_id = int(ros_cfg.get("domain_id", -1))
else:
    domain_id = -1

runtime_payload = {
    "default_agent_id": default_agent_id,
    "agent_id": default_agent_id,
    "robot_type": robot_type,
    "ros_domain_id": domain_id,
    "api_bind_host": "127.0.0.1",
    "api_port": 17447,
    "rules_file": str(Path(runtime_dir) / "routes.yaml"),
    "session_override_file": str(Path(runtime_dir) / "session_override.json"),
    "bridge_settings_file": str(bridge_settings_path),
}

with tempfile.NamedTemporaryFile(mode="w", dir=runtime_dir, delete=False, encoding="utf-8") as tmp:
    json.dump(runtime_payload, tmp, indent=2)
    tmp.write("\n")
os.replace(tmp.name, runtime_json_path)

lines = [
    f"ROS_CFG_DISTRO={quote(ros_distro)}",
    f"ROS_CFG_SETUP_SCRIPT={quote(ros_setup_script)}",
    f"ROS_CFG_DISCOVERY_RANGE={quote(discovery_range)}",
    f"ROS_CFG_DOMAIN_ID={quote(str(domain_id))}",
    f"ZENOH_AGENT_ID={quote(agent_id)}",
]
Path(env_file_path).write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

source "${ENV_FILE}"
rm -f -- "${ENV_FILE}"
trap - EXIT

# Resolve ROS 2 setup script
source "${APP_ROOT}/scripts/ros_setup.sh"
if [[ "${ROS_CFG_DISTRO}" == auto ]]; then ROS_CFG_DISTRO=''; fi
if ! RESOLVED_ROS_SETUP="$(find_ros_setup "${ROS_CFG_SETUP_SCRIPT}" "${ROS_CFG_DISTRO}")"; then
  echo "[start.sh] ERROR: Could not locate ROS 2 setup.bash" >&2
  exit 1
fi

echo "[start.sh] Sourcing ROS 2: ${RESOLVED_ROS_SETUP}"
set +u
source "${RESOLVED_ROS_SETUP}"
set -u

# Also source user ros2_ws overlay if present so custom message typesupports (e.g. px4_msgs, swift_msgs) are resolvable!
for overlay in \
  "${HOME}/ros2_ws/install/local_setup.bash" \
  "${HOME}/ros2_ws/install/setup.bash"; do
  if [[ -f "${overlay}" ]]; then
    echo "[start.sh] Sourcing ROS 2 overlay: ${overlay}"
    set +u
    source "${overlay}"
    set -u
    break
  fi
done

unset ROS_LOCALHOST_ONLY || true
export ROS_AUTOMATIC_DISCOVERY_RANGE="${ROS_CFG_DISCOVERY_RANGE}"
if [[ "${ROS_CFG_DOMAIN_ID}" -ge 0 ]]; then
  export ROS_DOMAIN_ID="${ROS_CFG_DOMAIN_ID}"
fi

BIN="${APP_ROOT}/build/zenoh_bridge_node"
if [[ ! -x "${BIN}" ]]; then
  echo "[start.sh] Binary missing, running pre_install.sh to build..."
  bash "${APP_ROOT}/scripts/pre_install.sh"
fi

echo "[start.sh] Launching zenoh_bridge_node (agent=${ZENOH_AGENT_ID}, discovery=${ROS_AUTOMATIC_DISCOVERY_RANGE})"
exec "${BIN}" --ros-args -r "__ns:=/${ZENOH_AGENT_ID}" -p "runtime_config_file:=${RUNTIME_JSON}"
