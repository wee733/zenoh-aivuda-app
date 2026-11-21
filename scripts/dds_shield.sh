#!/usr/bin/env bash
# Compatibility entry point for bridge installations. Only nft is elevated.
set -eu
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
exec /usr/bin/python3 -I "${here}/dds_shield.py" "$@"
