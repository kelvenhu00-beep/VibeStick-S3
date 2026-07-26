#!/usr/bin/env sh
set -u

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  printf 'PASS %s\n' "$1"
}

warn() {
  WARN_COUNT=$((WARN_COUNT + 1))
  printf 'WARN %s\n' "$1"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  printf 'FAIL %s\n' "$1"
}

require_command() {
  command_name="$1"
  help_text="$2"
  if command -v "$command_name" >/dev/null 2>&1; then
    pass "$command_name is available."
  else
    fail "$command_name is missing. $help_text"
  fi
}

if [ "$(uname -s 2>/dev/null)" = "Darwin" ]; then
  pass "macOS detected."
else
  fail "The packaged bridge and HUD installer currently requires macOS."
fi

require_command git "Install the Xcode Command Line Tools with: xcode-select --install"
require_command openssl "Install OpenSSL or make the system openssl command available."
require_command swiftc "Install the Xcode Command Line Tools with: xcode-select --install"

if command -v python3 >/dev/null 2>&1; then
  if python3 - <<'PY' >/dev/null 2>&1
import sys
raise SystemExit(0 if sys.version_info >= (3, 11) else 1)
PY
  then
    pass "Python >= 3.11 is available."
  else
    fail "Python >= 3.11 is required."
  fi
else
  fail "python3 is missing; install Python 3.11 or newer."
fi

if command -v idf.py >/dev/null 2>&1; then
  pass "ESP-IDF is loaded and idf.py is available."
elif [ -f "$HOME/esp/esp-idf/export.sh" ]; then
  warn "ESP-IDF is installed but not loaded. Run: . \"$HOME/esp/esp-idf/export.sh\""
else
  warn "ESP-IDF is not installed yet. It is required only to build and flash the firmware."
fi

for required_path in \
  "$ROOT_DIR/.env.example" \
  "$ROOT_DIR/firmware/sticks3/include/vibe_stick_secrets.example.h" \
  "$ROOT_DIR/firmware/sticks3/CMakeLists.txt" \
  "$ROOT_DIR/bridge/pyproject.toml"
do
  if [ -f "$required_path" ]; then
    pass "Found ${required_path#"$ROOT_DIR/"}."
  else
    fail "Repository file is missing: ${required_path#"$ROOT_DIR/"}"
  fi
done

printf '\nSummary: %s pass, %s warn, %s fail\n' "$PASS_COUNT" "$WARN_COUNT" "$FAIL_COUNT"

if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi

printf '%s\n' "Preflight passed. Continue with: ./scripts/setup.sh"
