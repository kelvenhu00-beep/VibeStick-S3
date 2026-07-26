#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT_DIR/bridge"

if [ -f "$ROOT_DIR/.env" ]; then
  set -a
  . "$ROOT_DIR/.env"
  set +a
fi

case "${VIBE_STICK_BRIDGE_TOKEN:-}" in
  ""|change-this-shared-token|paste-generated-token-here|changeme|change-me)
    printf '%s\n' "VIBE_STICK_BRIDGE_TOKEN is required because dev.sh exposes the bridge on 0.0.0.0." >&2
    printf '%s\n' "Generate one with: openssl rand -hex 32" >&2
    exit 1
    ;;
esac

PYTHONPATH="$ROOT_DIR/bridge/src" exec python3 -m vibe_stick --host 0.0.0.0 --port 8765
