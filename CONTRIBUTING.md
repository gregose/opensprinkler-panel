# Contributing

Thanks for your interest in improving this project! This is firmware for a
wall-mounted 3.5" resistive-touch CYD panel (LCDwiki ESP32-3248S035R) that
runs and steps OpenSprinkler stations over the controller's local HTTP API.

## Before You Start

- For anything more than a small fix, **open an issue first** to discuss the
  change. This avoids wasted effort on work that may not fit the project's
  direction.
- Keep pull requests **focused** — one logical change per PR is much easier to
  review than a large, mixed one.
- Read the [`docs/`](docs/) directory for architecture and context. In
  particular:
  - [`docs/03-architecture.md`](docs/03-architecture.md) — architecture and pin map
  - [`docs/02-opensprinkler-api.md`](docs/02-opensprinkler-api.md) — the OpenSprinkler HTTP API contract

## Development Setup

The project is built with [PlatformIO](https://platformio.org/). Install it via
the VS Code extension or the CLI (`pip install platformio`).

Common commands:

```sh
pio test -e native      # run native Unity unit tests
pio run -e cyd-35r      # build production firmware
```

## Architecture & Code Style

- **Hardware-independent logic** (domain models, API client, parsing, state
  machines) lives in `lib/*` as **pure C++** — no Arduino, no network. It uses
  injected transports so it can be covered by **native Unity tests**. See
  `lib/station_model` as the reference pattern.
- **Arduino / hardware glue** stays thin, in `src/`.
- Follow the existing code style in the files you touch.
- Update `docs/` when your change affects behavior, architecture, or the API
  contract.

## Testing Requirements

Every logic change must:

1. Keep native tests green: `pio test -e native`
2. Keep the firmware building: `pio run -e cyd-35r`

## Continuous Integration

CI runs on every pull request and must pass before merge. The jobs are:

- **Unit tests (native)** — `pio test -e native`
- **Bench tool tests (Python)** — tests for the developer bench/probe tooling
- **Mock controller tests (Python)** — contract tests against the mock
  OpenSprinkler controller
- **Build firmware** — builds the production (`cyd-35r`) and diagnostic
  (`cyd-35r-diag`) firmware images

Please run at least `pio test -e native` locally before opening a PR.

## Pull Request Checklist

See the [pull request template](.github/PULL_REQUEST_TEMPLATE.md). At a minimum,
describe what changed, how you tested it, and confirm that native tests pass.
