#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PYTHONPATH="$ROOT_DIR/bridge/src" exec python3 -m vibe_stick.config.wifi_profiles "$@"
