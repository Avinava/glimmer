# glimmer — collaboration notes for Claude Code

Hard-won facts from building this firmware, so future sessions don't re-walk
the same dead ends. Repo layout, house rules and CI: `AGENTS.md`. The
agent-facing card/MCP contract: `docs/AGENTS-GLIMMER.md`. Keep this tight.

## Hardware & build

- **Target**: GeekMagic SmallTV-Ultra — ESP8266 @ 160 MHz, 4 MB flash
  (`eagle.flash.4m1m.ld`, 1 MB LittleFS), ST7789 240×240. Backlight GPIO5 is
  **active-low PWM** (`analogWrite 0` = full bright). USB-C is power only.
- **No PSRAM, ~30 KB free heap** at idle. Never allocate a full 240×240 16bpp
  framebuffer (~115 KB). Literals in flash: `snprintf_P`/`PSTR`/`F()`.
- **Build**: `pio run -e nodemcuv2` (firmware), `pio run -e nodemcuv2 -t buildfs`
  (LittleFS image, `data/` → `/`), `pio test -e native` (host tests).
  Version: `-D FW_VERSION` in `platformio.ini`.
- **mDNS** `glimmer.local`; setup AP `glimmer-setup` (open) at `192.168.4.1`.
- **LittleFS files**: `/config.json` (settings), `/usage.bin` (history ring),
  `/weather.json` (last-good weather), `/fonts/*.vlw`, `/web/*`. A filesystem
  flash replaces all of them.

## Flashing (OTA over HTTP)

`POST /update`, form field exactly `firmware` or `filesystem`
(`ESP8266HTTPUpdateServer`). Firmware flash keeps the filesystem; filesystem
flash replaces it. `pio run -e ota` (espota) does **not** work — no ArduinoOTA.

Committed code: download CI builds from the rolling `latest` release instead
of building —
`https://github.com/Avinava/glimmer/releases/download/latest/{firmware,littlefs}.bin`.
Build locally only for uncommitted changes.

**Firmware only** (no `data/` changes): just
`curl -F "firmware=@firmware.bin" http://<ip>/update`.

**Firmware + filesystem, preferred: bake the config in (no AP).** The device
keeps its config and never drops to the setup AP.

```bash
IP=<device-ip>; S=$(mktemp -d)            # scratch dir — will hold secrets
curl -sf -o "$S/cfg.json" http://$IP/api/export
jq -e '.wifi_ssid' "$S/cfg.json"          # raw config.json (snake_case keys);
                                          # camelCase/"***" = no config file, stop
cp -R data "$S/fs" && cp "$S/cfg.json" "$S/fs/config.json"
pio run -e nodemcuv2
PLATFORMIO_DATA_DIR="$S/fs" pio run -e nodemcuv2 -t buildfs
curl -F "firmware=@.pio/build/nodemcuv2/firmware.bin"   http://$IP/update
# wait for it to come back (curl --max-time poll on /api/state), then:
curl -F "filesystem=@.pio/build/nodemcuv2/littlefs.bin" http://$IP/update
trash "$S"                                # delete the secrets copy
pio run -e nodemcuv2 -t buildfs           # rebuild the normal (config-free) image
```

`PLATFORMIO_DATA_DIR` must be absolute. Usage history and the weather cache
are still lost (they live on the FS); history refills over 7 days.

**Fallback: restore via the setup AP.** Flash `littlefs.bin` as-is; the
device reboots into `glimmer-setup`. Join it, then:

```bash
curl -X POST -H 'Content-Type: application/json' \
     --data-binary @/tmp/glimmer-config-backup.json http://192.168.4.1/api/import
```

It reboots onto normal Wi-Fi. `/api/export` returns the raw config **with
secrets** (Wi-Fi password, keys) and is not auth-gated — treat backups as
secrets.

## VLW smooth fonts — the critical gotchas

1. **Path**: `Display::useFont(name)` calls `tft.loadFont("fonts/<name>",
   LittleFS)`; TFT_eSPI prepends `/` and appends `.vlw` → `/fonts/<name>.vlw`.
2. **Filesystem**: one-arg `tft.loadFont(name)` defaults to SPIFFS — a silent
   fail here. Always pass `LittleFS`.
3. **Silent failure**: a missing file prints to Serial and returns; later
   `drawString` uses the last loaded font, or GLCD 5×7. This ate days.
4. **Glyphs**: ASCII 0x20–0x7E + `°` (U+00B0) + `·` (U+00B7) — `EXTRA` in
   `tools/genfonts.py`, now also applied to Silkscreen. No `…`: write `...`.
   Missing glyphs are skipped silently. Encode `·` as `"\xC2\xB7"` in C.
5. **Heap**: `Api::tlsGetStream` calls `Display::releaseFont()` (frees ~2–5
   KB) before every TLS handshake (~25 KB peak). Keep that.
6. **Shipped** (`data/fonts/`): DMMono-11, PixelifySans-14/22, Silkscreen-12/16,
   VT323-32/44/64/86. The generator's matrix has more (DMMono-9/10,
   Silkscreen-10, VT323-110) — don't ship unused ones (1 MB FS).

**VLW format** (verified against TFT_eSPI `Smooth_font.cpp`, big-endian):
header 24 B — u32 glyphCount, version (=11), fontSize, reserved, ascent,
descent. Per glyph 28 B, sorted by codepoint — u32 codepoint, height, width,
xAdvance, i32 dY (baseline→top, +up), i32 dX, u32 reserved. Then each
glyph's w×h grayscale bytes. Runbook: `.claude/skills/regenerate-fonts.md`.

## Display polarity / panel quirks

- `tft.invertDisplay(true)` is required; off = cream/washed colours. Exposed
  as `settings.invertDisplay` in case a panel revision differs.
- Theme `BG = 0x0000`: the design's `#0E0D12` reads cyan-grey on this panel.

## Channels & PARTIAL REDRAW DISCIPLINE

- `channel.h`: `Channel { name, enabled, draw, tick }`; `kChannels[]` in
  `main.cpp` registers them (order = rotation order). `recomputeActive()` runs
  every rotation tick, so settings toggles apply within one slide.
- `draw(ctx)` — full repaint on activation, may `Display::clear()`.
- `tick(ctx)` — 5 Hz while active. **Never** `Display::clear()` /
  `tft.fillScreen()`. File-static caches with sentinels (`-1`, `-2.f`) +
  per-region `paintX()` helpers that clear only their band; `draw()` seeds the
  cache. References: `ch_clock.cpp`, `ch_home.cpp`.
- **No animated transitions** — rotation is an instant cut.
- Indicator strip (y=230, 2 px): slide progress in the channel colour; solid
  amber (approval) / sky (question) on every screen while an agent waits.
- System screens (`drawSplash`, `drawConnecting`, `drawOtaProgress`) follow the
  same discipline; call `Display::resetSystemScreens()` from non-system entry
  points so the next one repaints its chrome.

## Fetch scheduler & heap gate

- No blocking "refresh all". `main.cpp` job table: claude, codex,
  codex-resets (hourly), weather, status-claude, status-openai. At most one
  job per loop pass, starts ≥ 3 s apart.
- A TLS job starts only when `ESP.getMaxFreeBlockSize() >= Api::kTlsFloor`
  (20 000); otherwise deferred 5 s and counted in `heap_refusals` — never an
  upstream failure. Weather is plain HTTP and skips the gate.
- `FetchPolicy::State` (`fetch_policy.h`): backoff 60→120→240→300 s,
  `Retry-After` honoured (≤ 6 h), credential latched bad only after **two**
  consecutive JSON 401/403. Errors surface only after 3 failures or a latched
  credential; until then stale data stays up.
- A job triggers a full redraw only when the data's *shape* changes
  (loading → valid → error); value changes are `tick()`'s job.
- Bodies are streamed into a filtered ArduinoJson parse (`tlsGetStream`) —
  never `getString()`.
- Claude per-model windows come from `limits[]` (`weekly_scoped`) or a fixed
  allowlist (`seven_day_opus`/`seven_day_sonnet`/`extra_usage`) — never a sweep
  of top-level keys (rotating internal codenames). Reset credits: `grants[]`
  under any top-level key (filter wildcard `"*"`); Codex
  `wham/rate-limit-reset-credits`.

## Credential states (`src/data/cred_state.h`)

- `NOT_SET / CHECKING / OK / EXPIRING / EXPIRED / REJECTED / BLOCKED`, derived
  by `Api::refreshCred()` after each fetch, on settings change, and every 10 s
  (a JWT crossing `exp` is noticed).
- **HTML 401/403 = an edge (Cloudflare) challenging the device, not a dead
  key** → `BLOCKED` after two in a row, never fed to the auth latch, retried
  every 15 min. Only JSON 401/403 ×2 → `REJECTED`.
- Codex `exp` from its JWT: `EXPIRING` < 72 h; `EXPIRED` skips the fetch.
  Claude's sessionKey has no expiry — it only fails.
- Surfaces, all in `src/channels/chrome.{h,cpp}`: `credCard` (no data),
  dimmed values + `credBanner` (data), `credLine` (Home/AI rows), `usageMeta`
  (status-bar meta).
- Device notices (`notice()` in `main.cpp`): 12 s `sys:*` cards through the
  attention queue, rate-limited per provider/reason (`CredState::NoticeLog`),
  not raised at night.
- No keys at all → `Setup` channel (hidden once either key is set, or both
  usage channels are off).

## Stale data & night modes

- `Api::isStale`: older than 3 × refresh interval (15 min floor) → dimmed hero
  + amber `STALE 14M` meta, never a takeover.
- `nightMode` 0 none, 1 dim, 2 clock (Night face only, no rotation), 3 dark
  (backlight off). Window = minutes past midnight; start == end disables.
- On the night face only these cards show (and wake a dark panel): `urgent`,
  a user's error card (agent `other`, not `sys:*`), and approvals/questions if
  `agentNightShow`. Others only light the strip (unseen in dark mode).

## Attention queue (agents, push, device notices)

- One queue (`attention_core.h`, 5 slots) behind `POST /push`, `GET /push`,
  `/push/clear`, `/hook`, MCP `push_card`/`clear_card`/`list_cards`, and the
  device's own `sys:*` notices. Upsert by id updates in place (no new
  interrupt unless the kind escalates); kind order = priority; a full queue
  never evicts an approval/input for something less important (push → 409).
- `/hook` takes the **trimmed** event from `tools/agents/glimmer-hook.sh`,
  never raw hook JSON (PostToolUse carries whole tool output; bodies > 2 KB
  are ignored). It always answers 200 so a hook can't block an agent.
- Items can be **hidden until `showAfter`**: Codex has no idle event, so its
  `Stop` queues a "your turn" card visible only after 60 s (`kCodexIdleS`);
  `UserPromptSubmit` clears it first if the user replies. Use
  `ordered(q, ord, now)` / `countWaiting(q, now)` for anything on screen.
- `Attention` channel = the full-screen card (holds ≤ 30 s, or until answered
  for approvals/inputs when `pinApprovals`; several holders cycle every 6 s).
  `Agents` channel = the list (≤ 3 rows; more → 2 rows + "+ N more").
- Card layout (`ch_attention.cpp`): centred `■ AGENT · project`, one VT323
  focal word/value (64, steps to 44), one muted detail line, kind-coloured
  status (`NEEDS YOU · 2M` / `WAITING · 2M`), queue dots or a time-left
  hairline.
- Enums are `K_*` / `SHOW_*` — `INPUT` is an Arduino macro.
- Keep `docs/AGENTS-GLIMMER.md` in sync with `attention_json.h`,
  `attention_core.h` and the MCP schema in `web.cpp`.

## Settings round-trip — four files

1. `src/core/storage.h` — field + default in `Settings`.
2. `src/core/storage.cpp` — `doc["snake_key"] | default` on load,
   `doc["snake_key"] = s.field` on save. Renaming a key needs a load-time
   fallback (see `tz_min`, `night_mode`).
3. `src/core/web.cpp` — `handleApiGetSettings` + `applyIfPresent` (camelCase).
   Runtime values are applied by `mainSettingsChanged()` after `Storage::save`.
4. `data/web/index.html` — input with `x-model="settings.fieldName"`.

## pixelBar

9 discrete segments with 1-px BG gaps, each fully lit or empty — **no partial
fills** (92% and 99% bars used to look like they crossed). Segment i lights
when `pct > i * 11.11`, so anything ≥ 89% is full.

## Common pitfalls

- **clangd noise** (missing `Arduino.h`, `uint16_t`…): no PlatformIO context.
  `pio run` is the truth.
- **Filesystem flash wipes config** unless baked in (see Flashing).
- **`ESP.restart()` doesn't reset the Wi-Fi RF** — after it the connect fails
  and the device strands in AP mode. Wi-Fi recovery is in-place, STA retries
  forever. At boot: 2 × 30 s attempts, then the setup AP, which retries the
  saved network every 180 s.
- **`Auth -1` / `Auth -2`** from a fetch = BearSSL handshake failed under heap
  pressure, not a bad key. Check `/api/state` `maxblk` / `heap_refusals`.
- **No sleep loops** in build/flash scripts; for "wait for the device" a short
  poll with `curl --max-time` is fine.

## Where the design lives

The visual design (palette, type, layouts) is in the
`/tmp/smalltv-design/smalltv/project/screens.jsx` archive — not in this repo.
Tokens are mirrored in `src/core/theme.h` (RGB565) and `data/fonts/*.vlw`
(design-spec pixel sizes).
