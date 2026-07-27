#!/usr/bin/env bash
#
# tools/flash.sh — download a merged CYD firmware image and flash it at 0x0.
#
# Two sources are supported:
#   * GitHub Actions CI artifacts (default): matched by --branch/--pr/--run-id.
#   * GitHub Releases (--release [tag]): the latest release, or a given tag.
# --diag selects the diagnostic bring-up firmware instead of production.

set -euo pipefail

# Use the repo-local flash toolchain venv if present (see tools/setup.sh).
# shellcheck source=tools/_venv.sh
source "$(dirname "${BASH_SOURCE[0]}")/_venv.sh"

artifact_prefix="cyd-35r-firmware"
branch=""
port=""
pr=""
repo=""
run_id=""
release=""
release_mode="no"
is_diag="no"
state_dir="${MON_STATE_DIR:-.serial-monitor}"

usage() {
  cat <<'EOF'
Usage: tools/flash.sh [--diag] [--branch <name> | --pr <number> | --run-id <id> | --release [tag]] [--port <device>] [--repo <owner/name>] [--state-dir <path>]

Download the merged firmware and flash it at 0x0.

By default the production firmware is downloaded from a GitHub Actions CI
artifact (cyd-35r-firmware-<sha>); pass --diag for the diagnostic bring-up
firmware (cyd-35r-diag-firmware-<sha>).

With --release [tag] the firmware is instead pulled from a GitHub Release:
with no tag (or 'latest') the newest release is used, otherwise the given tag.
--diag grabs the diag-merged-firmware.bin asset instead of the prod
merged-firmware.bin.

If tools/monitor.sh is running, it releases the port for the flash and resumes
afterward automatically.
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
  local successful_runs_filter='map(select(.status == "completed" and .conclusion == "success")) | .[].databaseId'

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
      --jq "$successful_runs_filter"
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
    --jq "$successful_runs_filter"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --diag)
      artifact_prefix="cyd-35r-diag-firmware"
      is_diag="yes"
      shift
      ;;
    --release)
      release_mode="yes"
      # Optional tag argument; skip if the next token is another flag or absent.
      if [[ -n "${2:-}" && "${2:0:2}" != "--" ]]; then
        release="$2"
        shift 2
      else
        shift
      fi
      ;;
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
    --state-dir)
      state_dir="${2:-}"
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

if [[ "$release_mode" == "yes" && ( -n "$branch" || -n "$pr" || -n "$run_id" ) ]]; then
  printf '%s\n' "--release cannot be combined with --branch/--pr/--run-id." >&2
  exit 1
fi

repo="$(resolve_repo)"

if [[ -z "$port" ]]; then
  port="$(detect_port)"
fi

download_dir="$(mktemp -d "$(osp_bench_tmp)/flash.XXXXXX")"
trap 'rm -rf "$download_dir"' EXIT

merged_firmware=""
flash_source=""

if [[ "$release_mode" == "yes" ]]; then
  # Pull the merged image from a GitHub Release instead of a CI artifact. The
  # release workflow publishes merged-firmware.bin (prod) and
  # diag-merged-firmware.bin (diag) as top-level release assets.
  if [[ "$is_diag" == "yes" ]]; then
    asset="diag-merged-firmware.bin"
  else
    asset="merged-firmware.bin"
  fi

  release_dir="$download_dir/release"
  mkdir -p "$release_dir"

  # No tag (or 'latest') -> newest release; otherwise the given tag.
  release_args=(-R "$repo" -p "$asset" -D "$release_dir" --clobber)
  if [[ -n "$release" && "$release" != "latest" ]]; then
    release_args=("$release" "${release_args[@]}")
  fi

  if ! gh release download "${release_args[@]}" >/dev/null 2>&1; then
    printf '%s\n' "Failed to download '$asset' from ${release:-latest} release of $repo." >&2
    exit 1
  fi

  merged_firmware="$release_dir/$asset"
  if [[ ! -f "$merged_firmware" ]]; then
    printf '%s\n' "Release asset '$asset' not found after download." >&2
    exit 1
  fi
  flash_source="release ${release:-latest} ($asset)"
else
  selected_run_id=""
  selected_artifact=""
  while IFS= read -r candidate_run_id; do
    [[ -n "$candidate_run_id" && "$candidate_run_id" != "null" ]] || continue

    candidate_dir="$download_dir/$candidate_run_id"
    mkdir -p "$candidate_dir"

    # Artifact names are suffixed with the commit SHA, so resolve the actual name
    # for this run by matching the prefix (production vs --diag).
    artifact_name="$(gh api "repos/$repo/actions/runs/$candidate_run_id/artifacts" \
      --jq '.artifacts[].name' 2>/dev/null | grep -m1 "^${artifact_prefix}-" || true)"
    [[ -n "$artifact_name" ]] || continue

    if ! gh run download "$candidate_run_id" -R "$repo" -n "$artifact_name" -D "$candidate_dir" >/dev/null 2>&1; then
      continue
    fi

    merged_firmware="$(find "$candidate_dir" -name merged-firmware.bin -print -quit)"
    if [[ -n "$merged_firmware" ]]; then
      selected_run_id="$candidate_run_id"
      selected_artifact="$artifact_name"
      break
    fi
  done < <(resolve_candidate_run_ids "$repo")

  if [[ -z "$selected_run_id" || -z "$merged_firmware" ]]; then
    printf '%s\n' "No successful ci.yml run with a ${artifact_prefix}-<sha> artifact was found." >&2
    exit 1
  fi

  run_id="$selected_run_id"
  flash_source="${selected_artifact:-$artifact_prefix} (run $run_id)"
fi

# Coordinate with a running tools/monitor.sh: ask it to release the port, wait
# for it to let go, flash, then clear the pause so it reconnects and re-captures
# the boot banner. If no monitor is running this is a brief, harmless no-op.
mkdir -p "$state_dir"
resume_monitor() { rm -f "$state_dir/pause"; }
trap resume_monitor EXIT
: > "$state_dir/pause"
for _ in $(seq 1 50); do
  [[ -e "$state_dir/active" ]] || break
  sleep 0.1
done

printf 'Flashing %s from %s to %s\n' "$flash_source" "$repo" "$port"
python3 -m esptool --chip esp32 --port "$port" write-flash 0x0 "$merged_firmware"
