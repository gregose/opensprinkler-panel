# 02 — OpenSprinkler Local HTTP API

Everything here was **verified against OpenSprinkler firmware source** (v2.2.1
unified firmware). The controller is reached at `http://<host>/<cmd>?...`. All
requests take a `pw` parameter. Responses are JSON.

> The panel is a **thin client**: it holds no authoritative state. It issues
> commands and then trusts the controller's status on a poll. This is what makes
> Wi-Fi loss and outside changes (someone using the app) self-correcting.

---

## Authentication

`pw` = **MD5 hex** of the device password (not the plaintext). Default device
password is `opendoor`; the user will set their own. Store the password (or its
MD5) from the setup flow in NVS and append `pw=<md5hex>` to **every** request.

Command endpoints return `{"result":<code>}`: **`1` = success**, `2` =
unauthorized (bad `pw`), `3` = mismatch, `16` = data missing, `17` = out of
range, `32` = not permitted. Treat anything other than `1` as a failure and
surface it (unauthorized → likely wrong password → re-provision).

---

## Endpoints used

### `GET /jn` — station names & attributes (configuration; fetched at startup / on wake, then cached)
Relevant fields:
```json
{
  "snames": ["Front Lawn", "Driveway Strip", "North Beds", ...],
  "stn_dis": [ <byte per board> ],     // disabled bitmask, LSB = lowest station on that board
  "masop":   [ <byte per board> ],     // master-1 association bitmask
  "masop2":  [ <byte per board> ],     // master-2 association bitmask
  "maxlen": 32
}
```
- **Station count** = `snames.length`.
- **Station `sid` is disabled** iff `(stn_dis[sid >> 3] >> (sid & 7)) & 1`.
- **Master/pump stations** are those with a bit set in `masop`/`masop2`. Omit disabled **and** master stations from the grid (the controller rejects running a master via `/cm`).

**Timing / caching.** `/jn` is **configuration**, not live state. Fetch it once at startup (and after the first-run config flow), then **cache** the result (names + enabled set + count) for the session. Do **not** poll it. The grid layout and Prev/Advance navigation are built from this cached list — never hardcoded — so any station count, disabled set, or rename is reflected automatically on the next launch. Station config only changes when the controller itself is reconfigured (adding an extender requires power-cycling the OS), so a fresh startup always has current config; no runtime re-fetch is needed.

### `GET /jc` — controller status (the poll, ~every 2 s while running)
Relevant fields:
```json
{
  "devt": 1719720000,                  // controller local epoch time ("now")
  "nbrd": 2,                           // number of 8-station boards
  "sbits": [ b0, b1, ..., 0 ],         // per-board on/off bitmask (trailing 0)
  "ps": [ [pid, rem, start, grp], ... ],// one entry per station, in sid order
  "RSSI": -68                          // controller's own Wi-Fi RSSI, dBm (ESP8266/OS-3.x only)
}
```
For each station `sid`, `ps[sid]` = `[pid, rem, start, grp]`:
- `pid` — program id driving it. **0 = idle.** Manual `/cm` runs show **99**. (You don't need to interpret the value beyond `!= 0`.)
- `rem` — **seconds remaining** (use this for the countdown). If queued but not yet started, it's the full duration.
- `start` — scheduled start epoch (`0` if not queued).
- `grp` — sequential group attribute (ignore).
- `RSSI` — the **controller's** Wi-Fi signal in dBm (negative). Present on the ESP8266-based OS v3. Use it for the top-bar **CTRL** indicator — it rides the same poll, no extra request. (The **PANEL** indicator is the ESP32's own `WiFi.RSSI()`, read locally.)

**Which station is currently on / to highlight:** the `sid` whose station bit is
set in `sbits` — `(sbits[sid >> 3] >> (sid & 7)) & 1`. Equivalently `ps[sid]`
with `pid != 0`, `rem > 0`, and `start <= devt`. Use `rem` for the countdown and
`devt` as "now".

> `/js` is a lighter alternative returning `{"sn":[0,1,...],"nstations":N}`
> (per-station on/off + count) but lacks `rem`. Prefer `/jc` for the poll since
> you need the countdown; `/js` is fine as a fallback.

### `GET /cm` — run / stop a single station (the core command)
Params: `sid` (**0-based**), `en` (`1` on / `0` off), `t` (seconds, required when `en=1`, range 1–64800), optional `qo` (queue option).

- Run station `n` for the run time: `GET /cm?pw=<md5>&sid=<n-1>&en=1&t=<RT>`
- Turn a station off: `GET /cm?pw=<md5>&sid=<n-1>&en=0`

**⚠ Critical quirk (verified in firmware):** issuing `en=1` on a station that is
**already running/queued does nothing** — the controller will not update the
timer. Therefore any operation that changes the running station's duration or
moves to a new station must **turn off first, then on**:

- **Advance / Prev / jump:** `sid=<current>&en=0` → then `sid=<target>&en=1&t=<RT>`
- **Extend (run time changed while running):** `sid=<current>&en=0` → then `sid=<current>&en=1&t=<RT'>`

Send the two calls back-to-back. Because default stations are **sequential**,
there may be a sub-second gap where nothing is on — that is acceptable (for the
blow-out use case the compressor tank absorbs it; do not attempt overlap).

**Master stations** return `not permitted` (32) — they're already filtered out of the grid.

### `GET /cv` — stop everything
`GET /cv?pw=<md5>&rsn=1` resets/stops all stations immediately. Use this for **Stop**.
(Other `/cv` params exist — `rbt=1` reboot, `en=` op enable, `rd=` rain delay — none are needed.)

---

## The client's job (put together)

**Boot / after config:** `GET /jn` → build the station list (names, filter
disabled + master). Then start polling.

**Poll loop (~2 s while running; slower when idle is fine):** `GET /jc` →
- find the on-station from `sbits`; set it as the highlighted/running station,
- set the countdown from its `ps[...].rem`,
- if nothing is on and the panel thought a station was running → it finished on the controller; go Idle (or, if Auto-advance is on and the panel initiated the run, advance — but prefer to let the panel's own timer drive auto-advance and use the poll only to reconcile).
- a failed/timed-out poll → signal-loss state; a subsequent success → clear it and re-sync.

**User actions** map to the calls above. After a command returns `result==1`,
optimistically update the UI; the next poll confirms.

---

## Networking notes

- **Short timeouts.** Set the HTTP client timeout to ~**1000–1500 ms**. A dead
  or unplugged controller must not freeze the UI for the default multi-second
  timeout. A timeout = treat as signal loss.
- **One in-flight request at a time** in the single-task model (see `03`);
  serialize command + poll so they don't interleave on the same socket.
- Prefer keeping the base URL (`http://<host>`) and `pw` assembled once from NVS.
- Local network only; no cloud/OTC. `host` may be an IP or mDNS name — support
  both (mDNS resolution can be flaky; an IP is the safe default to recommend to the user).

## Quick reference

| Action | Request |
|---|---|
| Load stations | `GET /jn?pw=…` |
| Poll status | `GET /jc?pw=…` |
| Run station n | `GET /cm?pw=…&sid=n-1&en=1&t=RT` |
| Turn off station n | `GET /cm?pw=…&sid=n-1&en=0` |
| Advance/Prev/Jump | off current → on target (`t=RT`) |
| Extend current | off current → on current (`t=RT'`) |
| Stop all | `GET /cv?pw=…&rsn=1` |
