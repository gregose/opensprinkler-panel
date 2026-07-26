#!/usr/bin/env bash
# tools/_venv.sh — sourced by the flash/serial bridge scripts (flash.sh,
# monitor.sh, seed-nvs.sh).
#
# If a repo-local .venv exists and no virtualenv is already active, activate it
# so `python3` / `esptool` resolve to the pinned Python 3.11 toolchain created
# by tools/setup.sh. This is what lets the scripts "just work" without the user
# manually activating a venv or referencing an absolute path.
#
# Safe no-op if .venv is absent (falls back to whatever python3 is on PATH) or
# if a venv is already active. Must be sourced, not executed.
if [[ -z "${VIRTUAL_ENV:-}" ]]; then
  _osp_repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  if [[ -f "$_osp_repo_root/.venv/bin/activate" ]]; then
    # The stock venv activate script is not written for `set -u`; disable it
    # around the source and restore afterwards.
    set +u
    # shellcheck disable=SC1091
    source "$_osp_repo_root/.venv/bin/activate"
    set -u
  fi
  unset _osp_repo_root
fi

# Repo-root scratch base for the bench tools. Everything transient a tool writes
# (artifact downloads, temp capture files, etc.) goes under <repo>/.bench-tmp
# (git-ignored) instead of the host's /tmp, so hardware bring-up/testing never
# pollutes the rest of the machine. Override with BENCH_TMPDIR if you must.
# Usage: dir="$(mktemp -d "$(osp_bench_tmp)/foo.XXXXXX")"
osp_bench_tmp() {
  local _root
  _root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  local _base="${BENCH_TMPDIR:-$_root/.bench-tmp}"
  mkdir -p "$_base"
  printf '%s\n' "$_base"
}
