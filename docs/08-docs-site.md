# 08 — Documentation site & user manual

How to maintain the **public** user manual and the hosted web flasher that ship
from this repo. This is a maintainer/agent reference; it is **not** published on
the site.

## The two documentation trees (don't mix them)

| Tree | Audience | Published? |
|---|---|---|
| [`docs/`](.) (this folder) | Developers / agents — the build & behavior source of truth | **No.** Never link `docs/` from the site or copy it into `site/`. |
| [`site/`](../site) | End users — install, configure, and use the panel | **Yes**, to GitHub Pages. |

Keep them separate. The public site must not expose dev-loop, agent, or bench
internals; when a user page needs to point at something advanced, link to the
GitHub repo generically rather than pasting `docs/` or `tools/` content.

## What the site is

`site/` is a [Just the Docs](https://just-the-docs.com/) (Jekyll) site:

- **Theme:** `just-the-docs`, pinned to **`0.12.0`** in [`site/Gemfile`](../site/Gemfile).
  It intentionally uses the **light** color scheme — *not* the app's dark/teal
  look. Do not switch it to a dark scheme.
- **Config:** [`site/_config.yml`](../site/_config.yml) — `title`, `theme`,
  `search_enabled`, the GitHub `aux_links`, `note`/`warning` `callouts`,
  `permalink: pretty`, and the site URL (see below).
- **Pages:** one Markdown file per page directly under `site/`, each with Just
  the Docs front matter.
- **Flasher:** a static browser flasher under [`site/flash/`](../site/flash)
  (see below).

### URLs (custom domain + project baseurl)

The site is served from the account's **custom domain** under a project path:

- `url: https://www.nullmethod.com`
- `baseurl: /opensprinkler-panel`

So the manual root is `https://www.nullmethod.com/opensprinkler-panel/` and the
flasher is `…/opensprinkler-panel/flash/`. Consequences:

- **Inside the site**, never hardcode the domain or `baseurl`. Use the
  `relative_url` filter for every internal link and image — for example
  `[Flashing]({{ '/flashing/' | relative_url }})` for a link and
  `![alt]({{ '/assets/img/screenshots/home-connected.png' | relative_url }})`
  for an image. This is what keeps links working under the `/opensprinkler-panel`
  baseurl — bare `/flashing/` links would 404.
- **Outside the site** (e.g. the repo `README.md`) use the full canonical URL
  `https://www.nullmethod.com/opensprinkler-panel/...`.
- If the canonical domain ever changes, update `url` in `_config.yml` and the
  absolute links in `README.md`; the in-site pages need no change because they
  use `relative_url`.

## Adding or editing a manual page

1. Create `site/<name>.md` with front matter:

   ```yaml
   ---
   title: Human Title
   layout: default
   nav_order: <n>
   ---
   ```

   `nav_order` controls the left-nav position; keep the sequence sane when
   inserting a page (renumber neighbors if needed).

2. Write the body. For internal links and images use `relative_url` (above).
   For emphasis boxes use the configured callouts:

   ```markdown
   {: .note }
   A helpful aside.

   {: .warning }
   Something that can bite the user.
   ```

3. Keep the content **user-facing** — no dev/agent/bench instructions.

Agents: mine panel behavior from [`01-ux-spec.md`](01-ux-spec.md) (manual
run/idle) and [`05-programs.md`](05-programs.md) (programs, queue, pause) — those
are authoritative. Don't invent behavior for the manual.

## The hosted web flasher (`site/flash/`)

- [`site/flash/index.html`](../site/flash/index.html) is an
  [ESP Web Tools](https://esphome.github.io/esp-web-tools/) install page. It has
  **no Jekyll front matter on purpose** — Jekyll passes files without front
  matter through verbatim, so the raw HTML/JS is served as-is. Do not add front
  matter to it.
- [`site/flash/manifest.json`](../site/flash/manifest.json) is **always-latest**:
  `version: "latest"`, `new_install_prompt_erase: true`, and the firmware part
  path is the absolute GitHub release asset
  `https://github.com/gregose/opensprinkler-panel/releases/latest/download/merged-firmware.bin`.
  **Keep that a `github.com` release URL** — it is *not* a Pages URL and must not
  be rewritten to the custom domain. The binary comes from `release.yml`.

## Build & deploy

[`.github/workflows/pages.yml`](../.github/workflows/pages.yml):

- **`build`** runs on `pull_request`, `push` to `main`, and `workflow_dispatch`
  when `site/**` (or the workflow) changes. It sets up Ruby via
  `ruby/setup-ruby`, `bundle install`s with `BUNDLE_GEMFILE=site/Gemfile`, then
  `bundle exec jekyll build -s site -d _site` with `JEKYLL_ENV=production`, and
  uploads the Pages artifact. This means **every PR that touches `site/`
  validates the Jekyll build** without deploying.
- **`deploy`** runs only on `push` to `main` / `workflow_dispatch` (the
  `github-pages` environment) and publishes the artifact.
- All actions are **SHA-pinned**; when bumping one, pin to the new release SHA
  (the same convention as [`ci.yml`](../.github/workflows/ci.yml)).
- GitHub Pages is enabled on the repo with the "GitHub Actions" source.

### Build verification

Use the PR's Pages **`build`** job as the authoritative Jekyll verification,
just like the repository's other CI builds. Do not install or modify a local
Ruby toolchain solely to validate site changes. Confirm the Actions job passes
before landing the PR.

`_site/` and Jekyll caches are git-ignored. **Never commit build output.**

### Gotcha: files without front matter are published verbatim

Any file under `site/` with no YAML front matter (like a stray `.md` note) is
copied straight into the built site and becomes publicly reachable, even if it
isn't in the nav. Keep maintainer notes — like this file — in `docs/`, not in
`site/`. (This is exactly why an earlier `site/.../CAPTURE.md` was moved here.)

## Screenshots

The manual's images live in
[`site/assets/img/screenshots/`](../site/assets/img/screenshots). This section
is the reference for what each one shows and how to re-shoot it. If an image
needs replacing, keep the **same filename** so the pages' references keep
working.

Capture the **on-panel** shots with the bench screenshot tool
([`tools/panel.py`](../tools/README.md)), which pulls a pixel-exact 480×320 PNG
over the panel's debug socket:

```bash
tools/panel.py --host <panel-ip> shot -o site/assets/img/screenshots/<name>.png
```

The panel's remote debug log (port 2323) must be enabled first (the "Enable
remote debug log" checkbox in the setup portal). Drive the UI into each state
below — by hand or with `tools/panel.py tap <x> <y>` — then capture. The fastest
way to reach the program / multi-station states deterministically is to point
the panel at the **mock controller** ([`mock_os.py`](mock_os.py)), which serves
24 stations and several programs, instead of a live controller.

| Filename | UI state to show | Fixture / how to get there |
|---|---|---|
| `home-connected.png` | Idle, connected: "Select a station" prompt, full station grid, top bar showing a teal droplet, controller name/IP, dim `0 mA`, P/C signal meters, and battery. | Boot connected to a controller (or `mock_os.py`). Don't start any station. |
| `manual-run.png` | A single station running: `STATION N` eyebrow, station name, large amber countdown, **Next ›** and **■ Stop** buttons, active pill highlighted, and live current in the top bar. | Tap a station pill from idle. Capture while the countdown is a clean value (e.g. after setting Run time to 1:00). |
| `programs-list.png` | Programs list: `PROGRAMS` header, several program rows each with name, next-run/zones/minutes meta line, Enable/Disable + **Run ›** buttons. | Tap **≡ Programs** from idle. Use `mock_os.py` so multiple programs (incl. a disabled one) are present. |
| `program-running.png` | Program running: left column `STATION N OF M`, station name, big countdown; right column live queue with a ✓ completed row, the current ▶ row, and upcoming rows; **Next ›**, **Pause**, **■ Stop**; live current in the top bar. | From the programs list tap **Run ›** on a multi-station program (the mock's >9-station program shows the windowed queue nicely). Capture a few stations in. |
| `setup-portal.png` | First-boot WiFiManager captive-portal landing menu (`OSPanel-Setup` with Configure / Info / Update / Exit). | Boot a freshly-flashed panel (empty NVS), join the `OSPanel-Setup` AP, and screenshot the portal landing page from the connecting device's browser. |
| `setup-portal-config.png` | The captive-portal configuration form: SSID + Password, OpenSprinkler host, device password, OTA password, sleep timeout, remote-debug-log fields. | Tap **Configure** in the portal and screenshot the form. Browser screenshot, not `panel.py`. |

Notes:

- **When to re-shoot:** Any PR that changes on-panel UI, especially the top
  status bar, home/idle, manual-run, programs list, or program-running screens,
  must re-capture the affected images above in the same PR using this procedure.
  Keep filenames stable.
- Keep on-panel captures at the native **480×320**; don't upscale or add device
  frames. The two captive-portal shots are browser screenshots and will differ
  in size — that's fine.
- Use descriptive, real-ish station/program names so the manual reads well.
- **Never capture real credentials** in the portal shots — leave the fields
  blank.
- After replacing an image, verify it renders on the built site and that its
  alt text on the referencing page still describes it accurately.
