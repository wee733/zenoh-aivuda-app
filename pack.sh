#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${ROOT}/dist"
mkdir -p "${OUT_DIR}"

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT
cp -R "${ROOT}"/{manifest.yaml,start.sh,assets,config,scripts,src,ui} "${STAGE_DIR}/"

ARCHIVE_NAME="$(python3 - "${STAGE_DIR}" <<'PY'
from pathlib import Path
import os
import re
import sys
import yaml

root = Path(sys.argv[1])
manifest = yaml.safe_load((root / "manifest.yaml").read_text(encoding="utf-8"))
app_id = os.environ.get("AIVUDA_PACKAGE_APP_ID")
if app_id:
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", app_id):
        raise ValueError("Invalid AIVUDA_PACKAGE_APP_ID")
    manifest["app_id"] = app_id
    if app_id == "app_zenoh_2026042116_ba01":
        manifest["name"] = "zenoh"
    (root / "manifest.yaml").write_text(yaml.safe_dump(manifest, sort_keys=False), encoding="utf-8")
cfg = yaml.safe_load((root / "config/default_config.yaml").read_text(encoding="utf-8"))
schema = yaml.safe_load((root / "config/config_schema.yaml").read_text(encoding="utf-8"))
assert isinstance(cfg, dict), "default_config.yaml must parse as object"
assert isinstance(schema, dict), "config_schema.yaml must parse as object"
try:
    import jsonschema
    jsonschema.validate(instance=cfg, schema=schema)
    print("[pack.sh] jsonschema validation passed!", file=sys.stderr)
except ImportError:
    print("[pack.sh] jsonschema not installed; basic YAML check passed.", file=sys.stderr)
print(f"{manifest['app_id']}-{manifest['version']}.tar.gz")
PY
)"

chmod +x "${STAGE_DIR}/start.sh" "${STAGE_DIR}/scripts/pre_install.sh" "${STAGE_DIR}/scripts/dds_shield.sh"

ARCHIVE="${OUT_DIR}/${ARCHIVE_NAME}"
tar --exclude='__pycache__' --exclude='*.pyc' -czf "${ARCHIVE}" -C "${STAGE_DIR}" .
echo "[pack.sh] Created ${ARCHIVE} ($(du -h "${ARCHIVE}" | cut -f1))"
tar -tzf "${ARCHIVE}"
