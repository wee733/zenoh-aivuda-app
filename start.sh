#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_PATH="${AIVUDA_APP_CONFIG_PATH:-${APP_ROOT}/config/default_config.yaml}"
RUNTIME_DIR="${AIVUDA_APP_RUNTIME_DATA_PATH:-${APP_ROOT}/runtime}"
mkdir -p "${RUNTIME_DIR}"

echo "[start.sh] zenoh bridge starting: app=${AIVUDA_APP_ID:-zenoh}@${AIVUDA_APP_VERSION:-dev}"
echo "[start.sh] config=${CONFIG_PATH}"
echo "[start.sh] runtime=${RUNTIME_DIR}"

# Parse AivudaOS sys/ros YAML config + persisted Web UI bridge_settings.json
ENV_FILE="${RUNTIME_DIR}/runtime_env.sh"
RUNTIME_JSON="${RUNTIME_DIR}/runtime_config.json"

python3 - "${CONFIG_PATH}" "${RUNTIME_DIR}" "${RUNTIME_JSON}" "${ENV_FILE}" <<'PY'
import json
import sys
from pathlib import Path
import yaml

cfg_path, runtime_dir, runtime_json_path, env_file_path = sys.argv[1:5]
cfg = yaml.safe_load(Path(cfg_path).read_text(encoding="utf-8")) or {}

sys_cfg = cfg.get("sys", {}) or {}
role_cfg = sys_cfg.get("role", {}) or {}
ros_cfg = cfg.get("ros", {}) or {}

role_id = role_cfg.get("id", 1)
robot_type = str(sys_cfg.get("robot_type", "fastlio2_rotor") or "fastlio2_rotor")
default_agent_id = f"uav{role_id}"

# Load persisted Web UI bridge settings if present (all Zenoh settings are managed in Web UI)
bridge_settings_path = Path(runtime_dir) / "bridge_settings.json"
bridge_settings = {}
if bridge_settings_path.is_file():
    try:
        bridge_settings = json.loads(bridge_settings_path.read_text(encoding="utf-8")) or {}
    except Exception as exc:
        print(f"[start.sh] Warning: failed to parse {bridge_settings_path}: {exc}", file=sys.stderr)

custom_agent_id = str(bridge_settings.get("agent_id", "") or "").strip().strip("/")
agent_id = custom_agent_id if custom_agent_id else default_agent_id
domain_prefix = str(bridge_settings.get("domain_prefix", "swarm") or "swarm").strip("/") or "swarm"
default_preset = str(bridge_settings.get("default_preset", "empty") or "empty").strip()
allow_unprefixed_local = bool(bridge_settings.get("allow_unprefixed_local", True))
liveliness_enabled = bool(bridge_settings.get("liveliness_enabled", True))

ros_distro = str(ros_cfg.get("distro", "auto") or "auto").strip()
ros_setup_script = str(ros_cfg.get("setup_script", "") or "").strip()
discovery_range = str(ros_cfg.get("discovery_range", "LOCALHOST") or "LOCALHOST").strip()
domain_id = int(ros_cfg.get("domain_id", -1))

runtime_payload = {
    "default_agent_id": default_agent_id,
    "agent_id": agent_id,
    "robot_type": robot_type,
    "domain_prefix": domain_prefix,
    "default_preset": default_preset,
    "allow_unprefixed_local": allow_unprefixed_local,
    "liveliness_enabled": liveliness_enabled,
    "api_bind_host": "127.0.0.1",
    "api_port": 17447,
    "rules_file": str(Path(runtime_dir) / "routes.yaml"),
    "session_override_file": str(Path(runtime_dir) / "session_override.json"),
    "bridge_settings_file": str(bridge_settings_path),
}

Path(runtime_json_path).write_text(json.dumps(runtime_payload, indent=2) + "\n", encoding="utf-8")

def sh_quote(val: str) -> str:
    return "'" + val.replace("'", "'\"'\"'") + "'"

lines = [
    f"ROS_CFG_DISTRO={sh_quote(ros_distro)}",
    f"ROS_CFG_SETUP_SCRIPT={sh_quote(ros_setup_script)}",
    f"ROS_CFG_DISCOVERY_RANGE={sh_quote(discovery_range)}",
    f"ROS_CFG_DOMAIN_ID={sh_quote(str(domain_id))}",
    f"ZENOH_AGENT_ID={sh_quote(agent_id)}",
]
Path(env_file_path).write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

source "${ENV_FILE}"

# Resolve ROS 2 setup script
RESOLVED_ROS_SETUP=""
if [[ -n "${ROS_CFG_SETUP_SCRIPT}" && -f "${ROS_CFG_SETUP_SCRIPT}" ]]; then
  RESOLVED_ROS_SETUP="${ROS_CFG_SETUP_SCRIPT}"
elif [[ "${ROS_CFG_DISTRO}" != "auto" && -f "/opt/ros/${ROS_CFG_DISTRO}/setup.bash" ]]; then
  RESOLVED_ROS_SETUP="/opt/ros/${ROS_CFG_DISTRO}/setup.bash"
else
  for d in jazzy lyrical kilted humble; do
    if [[ -f "/opt/ros/${d}/setup.bash" ]]; then
      RESOLVED_ROS_SETUP="/opt/ros/${d}/setup.bash"
      break
    fi
  done
fi

if [[ -z "${RESOLVED_ROS_SETUP}" ]]; then
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
