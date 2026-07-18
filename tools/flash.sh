#!/usr/bin/env bash

set -euo pipefail

artifact_name="cyd-35r-firmware"
branch=""
port=""
pr=""
repo=""
run_id=""

usage() {
  cat <<'EOF'
Usage: tools/flash.sh [--branch <name> | --pr <number> | --run-id <id>] [--port <device>] [--repo <owner/name>]

Download the merged firmware artifact from GitHub Actions and flash it at 0x0.
EOF
}

resolve_repo() {
  if [[ -n "$repo" ]]; then
    printf '%s\n' "$repo"
    return
  fi

  local origin
  origin="$(git config --get remote.origin.url)"
  origin="${origin#git@github.com:}"
  origin="${origin#https://github.com/}"
  origin="${origin%.git}"
  printf '%s\n' "$origin"
}

detect_port() {
  python3 - <<'PY'
import re
import sys
from serial.tools import list_ports

ports = list(list_ports.comports())
if not ports:
    sys.exit("No serial ports found. Pass --port explicitly.")

preferred = []
fallback = []
pattern = re.compile(r"(usb|uart|serial|acm|cp210|ch340|ftdi|silicon labs|wch)", re.I)
for port in ports:
    haystack = " ".join(
        value for value in [port.device, port.description, port.manufacturer, port.hwid] if value
    )
    if pattern.search(haystack):
        preferred.append(port.device)
    fallback.append(port.device)

devices = sorted(preferred or fallback)
print(devices[0])
PY
}

resolve_candidate_run_ids() {
  local active_repo="$1"
  local run_json_query='map(select(.status == "completed" and .conclusion == "success")) | .[].databaseId'

  if [[ -n "$run_id" ]]; then
    printf '%s\n' "$run_id"
    return
  fi

  if [[ -n "$pr" ]]; then
    branch="$(gh pr view "$pr" -R "$active_repo" --json headRefName --jq .headRefName)"
    gh run list \
      -R "$active_repo" \
      --workflow ci.yml \
      --branch "$branch" \
      --event pull_request \
      --json databaseId,status,conclusion \
      --limit 20 \
      --jq "$run_json_query"
    return
  fi

  if [[ -z "$branch" ]]; then
    branch="$(git rev-parse --abbrev-ref HEAD)"
  fi

  gh run list \
    -R "$active_repo" \
    --workflow ci.yml \
    --branch "$branch" \
    --json databaseId,status,conclusion \
    --limit 20 \
    --jq "$run_json_query"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --branch)
      branch="${2:-}"
      shift 2
      ;;
    --port)
      port="${2:-}"
      shift 2
      ;;
    --pr)
      pr="${2:-}"
      shift 2
      ;;
    --repo)
      repo="${2:-}"
      shift 2
      ;;
    --run-id)
      run_id="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'Unknown argument: %s\n\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -n "$branch" && -n "$pr" ]]; then
  printf '%s\n' "Use either --branch or --pr, not both." >&2
  exit 1
fi

repo="$(resolve_repo)"

if [[ -z "$port" ]]; then
  port="$(detect_port)"
fi

download_dir="$(mktemp -d)"
trap 'rm -rf "$download_dir"' EXIT

selected_run_id=""
merged_firmware=""
while IFS= read -r candidate_run_id; do
  [[ -n "$candidate_run_id" && "$candidate_run_id" != "null" ]] || continue

  candidate_dir="$download_dir/$candidate_run_id"
  mkdir -p "$candidate_dir"

  if ! gh run download "$candidate_run_id" -R "$repo" -n "$artifact_name" -D "$candidate_dir" >/dev/null 2>&1; then
    continue
  fi

  merged_firmware="$(find "$candidate_dir" -name merged-firmware.bin -print -quit)"
  if [[ -n "$merged_firmware" ]]; then
    selected_run_id="$candidate_run_id"
    break
  fi
done < <(resolve_candidate_run_ids "$repo")

if [[ -z "$selected_run_id" || -z "$merged_firmware" ]]; then
  printf '%s\n' "No successful ci.yml run with a ${artifact_name} artifact was found." >&2
  exit 1
fi

run_id="$selected_run_id"

printf 'Flashing run %s from %s to %s\n' "$run_id" "$repo" "$port"
python3 -m esptool --chip esp32 --port "$port" write_flash 0x0 "$merged_firmware"
