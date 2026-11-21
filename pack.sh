#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(python3 -c "import yaml; print(yaml.safe_load(open('${ROOT}/manifest.yaml'))['version'])")"
APP_ID="$(python3 -c "import yaml; print(yaml.safe_load(open('${ROOT}/manifest.yaml'))['app_id'])")"
OUT_DIR="${ROOT}/dist"
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

mkdir -p "${OUT_DIR}"

# Validate default_config.yaml against config_schema.yaml
python3 - "${ROOT}/config/default_config.yaml" "${ROOT}/config/config_schema.yaml" <<'PY'
import sys, yaml
cfg = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
schema = yaml.safe_load(open(sys.argv[2], encoding="utf-8"))
assert isinstance(cfg, dict), "default_config.yaml must parse as object"
assert isinstance(schema, dict), "config_schema.yaml must parse as object"
try:
    import jsonschema
    jsonschema.validate(instance=cfg, schema=schema)
    print("[pack.sh] jsonschema validation passed!")
except ImportError:
    print("[pack.sh] jsonschema not installed; basic YAML check passed.")
PY

cp "${ROOT}/manifest.yaml" "${STAGE_DIR}/"
cp "${ROOT}/start.sh" "${STAGE_DIR}/"
cp -r "${ROOT}/assets" "${STAGE_DIR}/"
cp -r "${ROOT}/config" "${STAGE_DIR}/"
cp -r "${ROOT}/scripts" "${STAGE_DIR}/"
cp -r "${ROOT}/src" "${STAGE_DIR}/"
cp -r "${ROOT}/ui" "${STAGE_DIR}/"

chmod +x "${STAGE_DIR}/start.sh" "${STAGE_DIR}/scripts/pre_install.sh"

ARCHIVE="${OUT_DIR}/${APP_ID}-${VERSION}.tar.gz"
tar -czf "${ARCHIVE}" -C "${STAGE_DIR}" .
echo "[pack.sh] Created ${ARCHIVE} ($(du -h "${ARCHIVE}" | cut -f1))"
tar -tzf "${ARCHIVE}"
