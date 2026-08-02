# CI/CD, Workflows & Releases

How this repo builds, tests, secures, and ships firmware. Compiling the ESP32
firmware happens in **GitHub Actions** — the local machine does not
cross-compile for the board; it downloads finished artifacts and flashes them
(see [`tools/README.md`](../tools/README.md)). The repo venv does, however,
provide the pinned PlatformIO for local **host** builds and tests — the LVGL
simulator (`pio run -e sim`, see [`docs/09-ui-simulator.md`](09-ui-simulator.md))
and the native unit tests (`pio test -e native`). This document is the
authoritative reference for the four workflows in `.github/workflows/` and the
tag-driven release process.

## Toolchain: one source of truth

The ESP32 platform/toolchain is pinned in `platformio.ini`
(`espressif32@7.0.1`), so it is identical everywhere by construction. The
surrounding host tools are pinned to the **same versions** across every
workflow — keep these in lockstep:

| Tool | Version | Where it's pinned |
|------|---------|-------------------|
| Python | `3.11` | `ci.yml`, `release.yml` (`PYTHON_VERSION`), `copilot-setup-steps.yml` |
| PlatformIO | `6.1.19` | `ci.yml`, `release.yml` (`PLATFORMIO_VERSION`), `copilot-setup-steps.yml`, `tools/requirements.txt` |
| esptool | `5.3.1` | `ci.yml`, `release.yml` (`ESPTOOL_VERSION`), `copilot-setup-steps.yml`, `tools/requirements.txt` |
| ESP32 platform | `espressif32@7.0.1` | `platformio.ini` (the single source) |

> If you bump any of these, change it in **all** of the files above in the same
> PR. This is enforced in CI: the `toolchain-consistency` job (below) runs
> `tools/check_toolchain_consistency.py`, which scrapes every location and fails
> the build if any tool resolves to more than one version.
>
> `zizmor.yml` also sets `PYTHON_VERSION`, but it is intentionally **not** in
> this set: that Python only hosts `pip install zizmor` (a prebuilt wheel) and
> is unrelated to compiling firmware, so it is free to move independently.

Every third-party action is **pinned by commit SHA** (with a trailing
`# vX.Y.Z` comment), not by tag, so a re-tagged action can't silently change
what runs. New workflows must follow the same convention, reusing the existing
pins where possible.

## Firmware environments

`platformio.ini` defines the buildable environments:

- **`cyd-35r`** — production firmware (full M6 UI, OTA, optional TCP log).
- **`cyd-35r-diag`** — diagnostic bring-up firmware (`src/diag/` only), shares
  the pin map + LVGL config via the `cyd_common` base section.
- **`native`** — host build of the pure-C++ `lib/*` logic for unit tests (no
  Arduino, no board).

Two build-time defines are injected via `-D … '"${sysenv.VAR}"'` and degrade to
an empty string (→ `"dev"` in C++) when the env var is unset, so ordinary
CI builds are unaffected:

- **`FW_GIT_SHA`** ← `GIT_SHA` — short commit SHA, shown on the boot screens and
  the web-portal brand line.
- **`FW_VERSION`** ← `FW_VERSION` — the release tag, set **only** by
  `release.yml`. The boot brand line renders `"<version> · <sha>"` for a tagged
  build and falls back to the bare sha/`"dev"` otherwise.

### Build order matters

Both `ci.yml` and `release.yml` build **production first, then diagnostic**.
This is not cosmetic: building `cyd-35r-diag` perturbs the shared `.pio/build`
output dir (it would wipe `cyd-35r`'s `bootloader.bin` before `merge_bin`
runs), so the production artifact must be built **and packaged** before the diag
build starts. Preserve this order in any new build job.

## Packaging: `tools/package-firmware.sh`

Both build workflows package each env the same way:

```
tools/package-firmware.sh <env> <out_dir> [--web]
```

It copies the individual parts and produces a single flashable image in
`<out_dir>`:

- `bootloader.bin`, `partitions.bin`, `firmware.bin`, `boot_app0.bin` — parts
- `merged-firmware.bin` — single 0x0 image (via `esptool merge-bin`)
- `espota.py` — bundled from the arduino-esp32 framework so `tools/ota.sh` can
  push OTA updates without a local PlatformIO install
- with `--web`: a `flash/` dir (ESP Web Tools page + manifest) for browser
  flashing

---

## Workflow 1 — `ci.yml` (build & test)

**Triggers:** `pull_request` (any branch) and `push` to `main`. Scoping push to
`main` avoids a duplicate run on every PR-branch push, since `pull_request`
already covers those.

**Permissions:** top-level `contents: read` (least privilege; artifact uploads
use the Actions API, which the read token already covers).

**Jobs (all `ubuntu-latest`):**

| Job | What it does |
|-----|--------------|
| `native-tests` | `pio test -e native` — the hardware-independent `lib/*` unit tests. |
| `tools-tests` | `python3 tools/test_panel.py` — bench probe tool tests (stdlib only). |
| `toolchain-consistency` | `python3 tools/check_toolchain_consistency.py` — fails if the pinned versions disagree across `platformio.ini`, the workflows, and this doc's toolchain table. |
| `mock-os-tests` | `python3 docs/test_mock_os.py` — OpenSprinkler emulator contract tests. |
| `firmware-build` | Builds + packages prod, then diag; uploads two artifacts. |

**`firmware-build` artifacts** (names suffixed with the short SHA so multiple
builds are distinguishable; `tools/flash.sh` and `tools/ota.sh` match them by
prefix):

- `cyd-35r-firmware-<sha>` — production, packaged with `--web`
- `cyd-35r-diag-firmware-<sha>` — diagnostic

These artifacts are what the local bench tools consume. See
[Flashing artifacts](#flashing-what-ci-built) below.

The job caches `~/.platformio` + `~/.cache/pip` keyed on `platformio.ini`'s
hash (`pio-fw-…`) to keep incremental builds fast.

## Workflow 2 — `release.yml` (tagged releases)

**Triggers:**

- `push` of a tag matching `v*` (e.g. `v1.0.0`) — the normal path.
- `workflow_dispatch` with a required `tag` input — re-package an existing tag
  without re-tagging.

**Permissions:** `contents: write` (needed to create the GitHub Release).

**Version resolution:** the `meta` step derives `version` from the tag
(`refs/tags/` stripped, or the dispatch input) and the short SHA like `ci.yml`.
Tags containing `-rc` (e.g. `v1.2.0-rc1`) are marked **prerelease**.

**Build:** same toolchain env and **prod-then-diag** order as `ci.yml`,
exporting both `GIT_SHA` and `FW_VERSION` on each `pio run` so the released
binaries carry the version string. Unlike `ci.yml`, **there is no build cache**
— a release builds from a clean runner so a poisoned cache can never taint a
published image (releases are rare; the extra minute is worth it).

**Publish:** assets are staged, then published with the **`gh` CLI**
(`gh release create` on first run, `gh release upload --clobber` on a dispatch
re-run) with `--generate-notes`. No third-party release action is used.

### Release asset set & naming

Diag assets are renamed so they don't collide with the production parts:

| Asset | Source |
|-------|--------|
| `merged-firmware.bin` | prod single 0x0 image |
| `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin` | prod individual parts |
| `diag-merged-firmware.bin` | diag merged image (renamed) |
| `diag-firmware.bin` | diag app partition (renamed) |
| `manifest.json` | version-pinned web-flasher manifest (see below) |
| `SHA256SUMS` | `sha256sum` over every `.bin` + `manifest.json` (relative names) |

### The release manifest uses an absolute URL

`flash/manifest.json` in the repo points at a **relative** path
(`../merged-firmware.bin`) and carries `version: "ci-artifact"` — that's for the
per-artifact browser-flash page. The **release** manifest is regenerated with
`version` = the tag and `parts[0].path` = the **absolute** release download URL:

```
https://github.com/gregose/opensprinkler-panel/releases/download/<tag>/merged-firmware.bin
```

so the web flasher installs the release image standalone, always tracking the
latest release. `new_install_prompt_erase: true` is preserved.

### Cutting a release

```bash
git tag v1.0.0
git push origin v1.0.0        # triggers release.yml
```

For a release candidate, tag `v1.0.0-rc1` (published as a prerelease). To
re-package an existing tag, run the **Release** workflow manually from the
Actions tab and pass the tag as input.

## Workflow 3 — `zizmor.yml` (workflow security)

Static analysis of our own Actions workflows with
[zizmor](https://docs.zizmor.sh) — important now that `release.yml` can publish
signed firmware, since a careless or compromised workflow is a supply-chain
risk. It catches template injection, credential persistence, cache poisoning,
over-broad token permissions, unpinned actions, and similar issues.

**Triggers:** `push` to `main` and every `pull_request` (not path-filtered — a
workflow can be affected indirectly, e.g. by a renamed script it calls).

**Permissions:** `security-events: write` (SARIF upload), `contents: read`,
`actions: read`.

**How it runs:** zizmor is pip-installed pinned to `ZIZMOR_VERSION` and run with
`--format=sarif`. The SARIF is uploaded to GitHub **code scanning**
(`github/codeql-action/upload-sarif`, SHA-pinned), so findings surface in the
Security tab and inline on PRs. Because zizmor exits `0` in SARIF mode, an
explicit **"Fail on findings"** step (jq over the SARIF results) gates the build
directly — PRs are blocked on any finding without needing a code-scanning
ruleset configured. The upload is `continue-on-error` so the workflow keeps
working during the window before code scanning is enabled; the gate remains the
authoritative pass/fail.

> Running zizmor locally (optional): `pipx run zizmor .github/workflows/`, or
> `uvx zizmor@<version> .github/workflows/`. Set `GH_TOKEN` to enable its
> online audits (e.g. verifying `uses:` refs resolve to real commits).

## Workflow 4 — `copilot-setup-steps.yml` (cloud agent env)

Prepares the Copilot cloud agent's ephemeral dev environment with the **same**
toolchain CI uses, so cloud sessions start with everything warm instead of
discovering it by trial and error. The job **must** be named
`copilot-setup-steps` for Copilot to pick it up.

**Triggers:** `workflow_dispatch`, plus `push`/`pull_request` limited to changes
to this file (so it's validated when edited).

It installs Python + PlatformIO + esptool, runs `pio pkg install` for the
`cyd-35r` and `native` envs, and **pre-warms** the libraries that milestones add
to `platformio.ini`.

> **Dependency rule (do not skip):** whenever you add or change a dependency in
> `platformio.ini` (`lib_deps`, `platform`, `platform_packages`,
> `test_framework`), you **must** also add it to the
> *Pre-warm milestone libraries (global)* step here, pinned to the same version
> range. The agent's runtime firewall may block `registry.platformio.org` during
> the work phase; `copilot-setup-steps.yml` runs **before** that firewall with
> full network, so pre-warming there guarantees the library resolves offline.
> Do **not** vendor third-party libraries into `lib/` to work around this.

---

## Flashing what CI built

The local bench tools never compile — they download a finished artifact or
release asset and write it over USB/OTA. Full reference in
[`tools/README.md`](../tools/README.md); the end-to-end bench runbook is
[`docs/06-hardware-validation-loop.md`](06-hardware-validation-loop.md).

```bash
# From a CI run (branch / PR / specific run):
./tools/flash.sh --branch my-branch     # latest successful ci.yml build for a branch
./tools/flash.sh --pr 42                 # latest build for a PR
./tools/flash.sh --diag                  # diagnostic firmware instead of production

# From a GitHub Release (uses `gh release download`):
./tools/flash.sh --release               # latest release (prod merged image)
./tools/flash.sh --release v1.0.0        # a specific tag
./tools/flash.sh --release --diag        # the diag-merged-firmware.bin asset
```

`flash.sh` writes `merged-firmware.bin` at `0x0` (which wipes NVS — back it up
with `tools/nvs.sh backup` first, or prefer OTA for routine iteration).
`tools/ota.sh` pushes the app partition wirelessly and preserves NVS. Browser
flashing works from either the per-artifact `flash/` page or a release's
`manifest.json`.

## At a glance

| Workflow | Trigger | Purpose | Key permission |
|----------|---------|---------|----------------|
| `ci.yml` | PR, push to `main` | Unit/tool/emulator tests + firmware build & artifacts | `contents: read` |
| `release.yml` | `v*` tag, manual dispatch | Build + publish a GitHub Release (prod + diag) | `contents: write` |
| `zizmor.yml` | PR, push to `main` | Security-audit the workflows themselves | `security-events: write` |
| `copilot-setup-steps.yml` | Edits to itself, manual | Pre-warm the Copilot cloud agent toolchain | `contents: read` |

## Conventions checklist for new/edited workflows

- Pin every action by commit SHA with a `# vX.Y.Z` comment; reuse existing pins.
- Set the least-privilege `permissions:` block the job actually needs.
- Set `persist-credentials: false` on `actions/checkout`.
- Never interpolate `${{ … }}` directly into a `run:` script — pass values via
  `env:` and reference the shell variable, to avoid template injection.
- Keep Python/PlatformIO/esptool versions identical to the table at the top —
  enforced by `tools/check_toolchain_consistency.py` in the
  `toolchain-consistency` CI job.
- Add any new `platformio.ini` dependency to `copilot-setup-steps.yml`.
- Run `zizmor .github/workflows/` (and `actionlint`) before pushing.
