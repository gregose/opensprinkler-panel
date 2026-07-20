#!/usr/bin/env bash

set -euo pipefail

# Use the repo-local flash toolchain venv if present (see tools/setup.sh).
# shellcheck source=tools/_venv.sh
source "$(dirname "${BASH_SOURCE[0]}")/_venv.sh"

# tools/ota.sh — push DEV_LOOP firmware over Wi-Fi via ArduinoOTA.
#
# Resolves the CI run the same way flash.sh does (--pr / --branch / --run-id),
# downloads the cyd-35r-dev-firmware artifact, then pushes the app-only
# firmware.bin via espota.py (bundled in the artifact by CI).  The board must
# be running a DEV_LOOP build that has ArduinoOTA active.
#
# NVS (Wi-Fi creds + os_host + os_pw_md5) is preserved across OTA because OTA
# rewrites only the app partition.  See docs/03 §"Wireless dev loop".

artifact_prefix="cyd-35r-dev-firmware"
branch=""
host="ospanel.local"
ota_pass=""
pr=""
repo=""
run_id=""

usage() {
  cat <<'EOF'
Usage: tools/ota.sh [--branch <name> | --pr <number> | --run-id <id>]
                    [--host <ip-or-hostname>] [--ota-pass <password>]
                    [--repo <owner/name>]

Push the cyd-35r-dev firmware artifact over Wi-Fi using ArduinoOTA.

The board must be running a DEV_LOOP build (ArduinoOTA active).  One-time
bootstrap: flash merged-firmware.bin via tools/flash.sh first.

Options:
  --branch <name>      Use the most recent successful CI run on this branch.
                       Defaults to the current git branch.
  --pr <number>        Use the most recent successful CI run for this PR.
  --run-id <id>        Use a specific workflow run ID.
  --host <host>        Device hostname or IP (default: ospanel.local).
  --ota-pass <pw>      OTA password stored in device NVS (ota_pass key).
                       Omit if the device was provisioned without a password.
  --repo <owner/name>  Override the GitHub repository (default: from git remote).
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
    --branch)
      branch="${2:-}"
      shift 2
      ;;
    --host)
      host="${2:-}"
      shift 2
      ;;
    --ota-pass)
      ota_pass="${2:-}"
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

download_dir="$(mktemp -d)"
trap 'rm -rf "$download_dir"' EXIT

selected_run_id=""
firmware_bin=""
espota_py=""

while IFS= read -r candidate_run_id; do
  [[ -n "$candidate_run_id" && "$candidate_run_id" != "null" ]] || continue

  candidate_dir="$download_dir/$candidate_run_id"
  mkdir -p "$candidate_dir"

  # Artifact names are suffixed with the commit SHA; match by prefix.
  artifact_name="$(gh api "repos/$repo/actions/runs/$candidate_run_id/artifacts" \
    --jq '.artifacts[].name' 2>/dev/null | grep -m1 "^${artifact_prefix}-" || true)"
  [[ -n "$artifact_name" ]] || continue

  if ! gh run download "$candidate_run_id" -R "$repo" -n "$artifact_name" -D "$candidate_dir" >/dev/null 2>&1; then
    continue
  fi

  firmware_bin="$(find "$candidate_dir" -name firmware.bin -print -quit)"
  espota_py="$(find "$candidate_dir" -name espota.py -print -quit)"

  if [[ -n "$firmware_bin" && -n "$espota_py" ]]; then
    selected_run_id="$candidate_run_id"
    break
  fi
done < <(resolve_candidate_run_ids "$repo")

if [[ -z "$selected_run_id" || -z "$firmware_bin" || -z "$espota_py" ]]; then
  printf '%s\n' "No successful ci.yml run with a ${artifact_prefix}-<sha> artifact (including firmware.bin + espota.py) was found." >&2
  exit 1
fi

printf 'OTA: run %s  artifact %s  host %s\n' "$selected_run_id" "$artifact_prefix" "$host"

espota_args=(-i "$host" -f "$firmware_bin")
if [[ -n "$ota_pass" ]]; then
  espota_args+=(-a "$ota_pass")
fi

python3 "$espota_py" "${espota_args[@]}"
