#!/usr/bin/env bash
# tools/setup.sh — one-time bootstrap of the local host toolchain.
#
# Creates a repo-local .venv using Python >=3.10 (preferring 3.11 to match CI)
# and installs the pinned tools from tools/requirements.txt. Idempotent — safe
# to re-run to pick up requirement changes.
#
# This venv now provides the pinned PlatformIO too, so you can run the HOST
# builds/tests locally: the LVGL sim (`pio run -e sim`, see
# docs/09-ui-simulator.md) and the native unit tests (`pio test -e native`).
# Compiling the ESP32 firmware still happens in cloud CI.
#
# After this, tools/flash.sh, monitor.sh and seed-nvs.sh auto-activate .venv
# (see tools/_venv.sh); you do not need to activate it yourself. For pio,
# either `source .venv/bin/activate` or invoke `./.venv/bin/pio`.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_dir="$repo_root/.venv"
req="$repo_root/tools/requirements.txt"

# Locate a Python >=3.10 interpreter, preferring 3.11 to match CI.
find_python() {
  local candidate
  for candidate in python3.11 python3.12 python3.10 python3; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    if "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info[:2] >= (3, 10) else 1)'; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

py="$(find_python || true)"
if [[ -z "$py" ]]; then
  cat >&2 <<'EOF'
No Python >=3.10 found on PATH. Install one first, e.g. on macOS:

  brew install python@3.11

then re-run ./tools/setup.sh.
EOF
  exit 1
fi

echo "Using $("$py" --version) ($py)"

if [[ ! -x "$venv_dir/bin/python" ]]; then
  echo "Creating venv at $venv_dir"
  "$py" -m venv "$venv_dir"
fi

# shellcheck disable=SC1091
source "$venv_dir/bin/activate"
python -m pip install --quiet --upgrade pip
python -m pip install --quiet -r "$req"

echo "Host toolchain ready:"
python -m platformio --version 2>/dev/null | sed 's/^/  /' || true
python -m esptool version 2>/dev/null | grep -i "^esptool" || python -m esptool version 2>/dev/null | head -1 || true
python -c "import serial; print('  pyserial', serial.__version__)"
echo "Done. tools/flash.sh, monitor.sh and seed-nvs.sh will use this .venv automatically."
echo "For host builds/tests: 'source .venv/bin/activate' then 'pio run -e sim' / 'pio test -e native' (or './.venv/bin/pio ...')."
