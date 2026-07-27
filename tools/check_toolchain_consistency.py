#!/usr/bin/env python3
"""Enforce toolchain-version consistency across the repo.

The Python / PlatformIO / esptool / ESP32-platform versions are duplicated in
several files by necessity (GitHub Actions env blocks, the Copilot setup
workflow, platformio.ini, the local venv requirements, and the toolchain table
in docs/07-ci-cd-and-releases.md). They MUST stay identical everywhere. This
script is the machine check for that rule: it scrapes every known location,
groups the findings per tool, and fails if any tool resolves to more than one
version.

It deliberately hardcodes NO version numbers — bumping a version just has to be
done consistently, and this check enforces the "consistently" part. Stdlib
only, so it runs anywhere with no install (matching the other tools/*.py).

Run:  python3 tools/check_toolchain_consistency.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class Finding:
    def __init__(self, tool: str, version: str, source: str) -> None:
        self.tool = tool
        self.version = version
        self.source = source


# Each rule: (tool, file, compiled regex with one capture group). Every regex
# captures a literal version; interpolations like ${{ env.X }} never match, so
# an env-referencing `python-version: ${{ env.PYTHON_VERSION }}` is correctly
# ignored (its literal source is the env block instead).
def _rules() -> list[tuple[str, str, re.Pattern[str]]]:
    ver = r'["\']?(\d+(?:\.\d+)+)["\']?'
    # Only the workflows that actually BUILD/PACKAGE firmware are constrained.
    # zizmor.yml is deliberately excluded: its Python is just the host that runs
    # `pip install zizmor` (a prebuilt wheel), unrelated to the firmware
    # toolchain, so pinning it to match PlatformIO's Python would be arbitrary
    # coupling. It pins ZIZMOR_VERSION for its own reproducibility instead.
    env_rules = []
    for wf in ("ci.yml", "release.yml"):
        path = f".github/workflows/{wf}"
        env_rules += [
            ("python", path, re.compile(rf"^\s*PYTHON_VERSION:\s*{ver}", re.M)),
            ("platformio", path, re.compile(rf"^\s*PLATFORMIO_VERSION:\s*{ver}", re.M)),
            ("esptool", path, re.compile(rf"^\s*ESPTOOL_VERSION:\s*{ver}", re.M)),
        ]
    setup = ".github/workflows/copilot-setup-steps.yml"
    # docs/07's "one source of truth" table restates every version in prose, so
    # it drifts just like the workflows if not enforced. Anchor to the table
    # rows (backtick-wrapped) and the ESP32 pin so the doc must be updated in the
    # same PR as any bump.
    doc = "docs/07-ci-cd-and-releases.md"
    dver = r"`(\d+(?:\.\d+)+)`"
    return env_rules + [
        # Copilot cloud-agent setup: literal python-version + pip installs.
        ("python", setup, re.compile(rf"^\s*python-version:\s*{ver}", re.M)),
        ("platformio", setup, re.compile(rf"platformio=={ver}")),
        ("esptool", setup, re.compile(rf"esptool=={ver}")),
        # Local flash/serial venv — pinned to match CI (see the file header).
        ("esptool", "tools/requirements.txt", re.compile(rf"^esptool=={ver}", re.M)),
        # The single source of truth for the ESP32 platform.
        ("esp32-platform", "platformio.ini", re.compile(rf"espressif32@{ver}")),
        # docs/07 toolchain table + the ESP32 pin mentioned in its prose.
        ("python", doc, re.compile(rf"^\|\s*Python\s*\|\s*{dver}", re.M)),
        ("platformio", doc, re.compile(rf"^\|\s*PlatformIO\s*\|\s*{dver}", re.M)),
        ("esptool", doc, re.compile(rf"^\|\s*esptool\s*\|\s*{dver}", re.M)),
        ("esp32-platform", doc, re.compile(rf"espressif32@{ver}")),
    ]


# Minimum number of independent sources each tool must be found in, so a typo
# or refactor that stops a regex from matching is caught instead of silently
# reducing coverage to a trivially-consistent single value.
MIN_SOURCES = {
    "python": 4,
    "platformio": 4,
    "esptool": 5,
    "esp32-platform": 3,
}


def collect() -> list[Finding]:
    findings: list[Finding] = []
    for tool, rel, pattern in _rules():
        path = REPO_ROOT / rel
        if not path.exists():
            print(f"ERROR: expected file is missing: {rel}", file=sys.stderr)
            sys.exit(2)
        text = path.read_text(encoding="utf-8")
        matches = pattern.findall(text)
        for version in matches:
            findings.append(Finding(tool, version, rel))
    return findings


def main() -> int:
    findings = collect()

    by_tool: dict[str, list[Finding]] = {}
    for f in findings:
        by_tool.setdefault(f.tool, []).append(f)

    ok = True

    # 1) Every tool must appear in at least the expected number of sources.
    for tool, minimum in MIN_SOURCES.items():
        found = by_tool.get(tool, [])
        if len(found) < minimum:
            ok = False
            print(
                f"ERROR: {tool}: found {len(found)} source(s), expected >= {minimum}. "
                f"A version reference may have moved or a regex needs updating.",
                file=sys.stderr,
            )

    # 2) Every tool must resolve to exactly one version across all sources.
    for tool in sorted(by_tool):
        found = by_tool[tool]
        versions = sorted({f.version for f in found})
        if len(versions) > 1:
            ok = False
            print(f"ERROR: {tool}: inconsistent versions {versions}:", file=sys.stderr)
            for f in sorted(found, key=lambda x: (x.version, x.source)):
                print(f"    {f.version:12} {f.source}", file=sys.stderr)

    if not ok:
        print(
            "\nToolchain versions must match across platformio.ini, the "
            "workflows, and the docs/07 toolchain table "
            "(see docs/07-ci-cd-and-releases.md).",
            file=sys.stderr,
        )
        return 1

    print("Toolchain versions are consistent:")
    for tool in sorted(by_tool):
        version = by_tool[tool][0].version
        count = len(by_tool[tool])
        print(f"  {tool:16} {version:12} ({count} source(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
