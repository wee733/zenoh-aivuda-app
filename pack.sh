#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${ROOT}/dist"
mkdir -p "${OUT_DIR}"

ARCHIVE_NAME="$(python3 - "${ROOT}" <<'PY'
from pathlib import Path
import sys
import yaml

root = Path(sys.argv[1])
manifest = yaml.safe_load((root / "manifest.yaml").read_text(encoding="utf-8"))
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

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT
cp -R "${ROOT}"/{manifest.yaml,start.sh,assets,config,scripts,src,ui} "${STAGE_DIR}/"
chmod +x "${STAGE_DIR}/start.sh" "${STAGE_DIR}/scripts/pre_install.sh" "${STAGE_DIR}/scripts/dds_shield.sh"

ARCHIVE="${OUT_DIR}/${ARCHIVE_NAME}"
tar --exclude='__pycache__' --exclude='*.pyc' -czf "${ARCHIVE}" -C "${STAGE_DIR}" .
echo "[pack.sh] Created ${ARCHIVE} ($(du -h "${ARCHIVE}" | cut -f1))"
tar -tzf "${ARCHIVE}"
