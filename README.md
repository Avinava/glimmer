<p align="center">
  <img src="./docs/hero.jpg" alt="three glimmer devices on a desk, showing CODEX usage, HOME dashboard, and CLAUDE usage" width="100%">
</p>

# glimmer

> A pixel-art always-on desk widget. Custom firmware for the **GeekMagic
> SmallTV-Ultra** (and the **ESP32 "Cheap Yellow Display"**) that rotates
> through glanceable channels — Claude / Codex usage, clock, weather, push
> cards — with crisp retro typography.

---

## What it shows

| Channel | What |
|---|---|
| **Home** | At-a-glance clock + weather + Claude/Codex usage meters + 24-hour timeline |
| **Clock** | Big VT323 digital clock, day-of-week, greeting |
| **Weather Now** | Hero temp, feels/humidity/wind, 3-day mini cards |
| **5-day Forecast** | Range-bar rows showing min/max + condition |
| **Claude usage** | 5-hour window % + weekly % + reset countdowns |
| **Codex usage** | Primary % + secondary % + credits/reset |
| **AI Today** | Combined Claude+Codex card with 7-day mini chart |
| **Info** | IP / SSID / signal / uptime / heap / CPU / firmware |
| **Push cards** | One-shot notification cards via `POST /push` |

All channels render via region-based partial repaints — no flickering
between data updates.

## Quick start — flash the prebuilt binaries (no toolchain)

Every push to `main` is built by CI and published to the
**[`latest`](https://github.com/Avinava/glimmer/releases/tag/latest)**
release, so you don't need PlatformIO to flash a device:

```bash
# Grab the latest CI-built images
curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/littlefs.bin
curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin

# Flash a freshly-stocked SmallTV-Ultra (over your home LAN). Filesystem FIRST:
DEVICE_IP=<find via arp or device screen>
curl -F "filesystem=@littlefs.bin" http://$DEVICE_IP/update
curl -F "firmware=@firmware.bin"   http://$DEVICE_IP/update

# Device reboots into glimmer's setup AP. Connect to "glimmer-setup" Wi-Fi
# (open, no password) and visit http://192.168.4.1/ to enter your home
# Wi-Fi credentials.
```

Want a specific release instead of the rolling latest? Each `v*` tag has its
own assets under **[Releases](https://github.com/Avinava/glimmer/releases)**.

Prefer to build from source? See [Development](#development) below
(`pio run` + `pio run -t buildfs`).

On the **ESP32 Cheap Yellow Display** USB is data-wired, so you flash
directly over USB (no OTA needed):

```bash
pio run -e cyd -t buildfs   # build LittleFS image (fonts + web UI)
pio run -e cyd              # build firmware
pio run -e cyd -t upload    # flash firmware over USB
pio run -e cyd -t uploadfs  # flash filesystem over USB
# Then connect to the "glimmer-setup" AP → http://192.168.4.1/ as above.
```

> Note: `-t uploadfs` rewrites the whole LittleFS partition, which wipes
> `/config.json`. To keep your settings across a filesystem update, back up
> first (`curl http://<device-ip>/api/export`) and restore after via the
> setup AP (`POST /api/import`).

For the full flashing dance (including the one-time "get the stock
firmware onto your Wi-Fi first" step), see **[FLASHING.md](./FLASHING.md)**.

## Hand this to Claude Code

You can let [Claude Code](https://claude.com/code) drive the flash for
you end-to-end — it will detect whether the device is brand-new (stock
firmware) or already on glimmer, walk you through joining the right
Wi-Fi AP, verify each step before continuing, build, OTA-flash, and
restore your config.

### From a clone of this repo

```
> /skill flash-device
```

### From anywhere (paste this prompt)

> Please flash glimmer firmware onto my GeekMagic SmallTV-Ultra.
> The skill at
> **https://github.com/Avinava/glimmer/blob/main/.claude/skills/flash-device.md**
> has the full walkthrough — fetch it, then walk me through:
>
> 1. Detect device state (brand-new stock vs already on glimmer).
> 2. Help me join the correct Wi-Fi AP (`GIFTV` for stock,
>    `glimmer-setup` after first glimmer flash).
> 3. Verify connectivity at every step (curl `/api/state` etc.) —
>    don't assume; always confirm with me before each handoff.
> 4. Download the prebuilt `firmware.bin` + `littlefs.bin` from the
>    [`latest`](https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin)
>    release (no toolchain needed; build locally only for unpushed changes).
> 5. OTA-flash filesystem first, then firmware.
> 6. After full flash, restore my config from backup OR walk me
>    through first-time setup (Wi-Fi → tokens → channels → personalization).
>
> My device's current location: `<plugged in next to me / on the LAN at
> <ip>/glimmer.local>`. My laptop OS: `<macOS / Linux / Windows>`.

Claude will read the skill file, build the artifacts, and run the OTA
dance with you. Don't run any of the curl commands yourself unless
Claude asks — the order matters (filesystem flash wipes `/config.json`,
needs the AP-rejoin step to recover).

## Hardware

glimmer supports two targets. The ESP32 port is layered behind
`#if defined(ESP32)`, so the original ESP8266 build is unchanged.

### GeekMagic SmallTV-Ultra — original target (`-e nodemcuv2`)

- **MCU**: ESP8266, 80–160 MHz, ~30 KB free RAM
- **Display**: 240×240 ST7789V IPS TFT (requires `invertDisplay(true)`)
- **Backlight**: PWM on GPIO5, **active-low** (0 = full bright, 1023 = off)
- **Flash**: 4 MB total → 3 MB sketch / 1 MB LittleFS (`eagle.flash.4m1m.ld`)
- **USB-C**: power only — no data wired to MCU (flash over Wi-Fi / OTA)
- **Network**: Wi-Fi 2.4 GHz only

### ESP32 "Cheap Yellow Display" — ESP32-2432S028R (`-e cyd`)

- **MCU**: ESP32-WROOM-32, dual-core 240 MHz (mbedTLS instead of BearSSL)
- **Display**: 320×240 ILI9341 (landscape); backlight **active-high** PWM on GPIO21
- **Flash**: 4 MB (`huge_app.csv` → 3 MB app / ~1 MB LittleFS)
- **USB**: data wired — flash directly over USB (`-t upload` / `-t uploadfs`)
- **Extras**: XPT2046 resistive touch + microSD slot present but unused
- **Network**: Wi-Fi 2.4 GHz only

### Known limitations

- ESP8266 BearSSL TLS is tight on heap — glimmer drops the VLW font
  cache before TLS calls (`Display::releaseFont()`). Don't add more
  long-lived heap allocations along the Claude/Codex fetch path.
- No PSRAM, no DMA double-buffer — channel rotation is a ~80 ms instant
  cut. Within-channel updates are region-based, smooth.
- Display panel needs the inversion bit; the web UI exposes
  `Invert colors` toggle in case a panel revision differs.

## Development

### Build

```bash
pio run -e nodemcuv2              # firmware  (ESP8266 / SmallTV-Ultra)
pio run -e nodemcuv2 -t buildfs   # filesystem
pio run -e cyd                    # firmware  (ESP32 / Cheap Yellow Display)
pio run -e cyd -t buildfs         # filesystem
```

### Regenerate fonts

The repo ships with pre-built VLW bitmap fonts (`data/fonts/*.vlw`)
sized to match the design spec. To add a size or family, edit
`tools/genfonts.py`'s `FONT_MATRIX`, drop the TTF in `tools/ttf/`, and:

```bash
pip install freetype-py
python3 tools/genfonts.py
pio run -e nodemcuv2 -t buildfs
```

Or trigger via Claude Code:

```
> /skill regenerate-fonts
```

### Add a channel

1. Create `src/channels/ch_<name>.cpp` with `chXxxEnabled`, `chXxxDraw`,
   and (optional) `chXxxTick`.
2. Add a row to `kChannels[]` in `src/main.cpp`.
3. (Optional) Add a `showXxx` toggle in `src/core/storage.{h,cpp}`,
   `src/core/web.cpp`, and `data/web/index.html`.

See `src/channels/ch_clock.cpp` for the reference partial-redraw
implementation.

## Getting your tokens

glimmer reads your usage by replaying your own browser session against the
same private endpoints claude.ai and chatgpt.com use for their dashboards.
You extract each credential from your browser's DevTools, then **set it on
the Tokens page in the device web UI** — open `http://glimmer.local/` (or
`http://192.168.4.1/` while the device is in setup-AP mode) and go to
**Settings → Tokens**.

### Claude — `sessionKey` cookie

**Get it:**

1. Open DevTools on `claude.ai` → **Application → Cookies → `https://claude.ai`**
2. Copy the value of the `sessionKey` cookie (starts with `sk-ant-sid02-…`)

**Set it:** paste it on the Tokens page under **Claude → Session key**.

### Codex — Bearer token + device ID

The token must come from a **`backend-api`** request, *not* a CDN/asset
request — CDN requests don't carry an `authorization` header.

**Get it:**

1. Open DevTools on `chatgpt.com` → **Network** tab
2. Filter for `backend-api` and click any request (conversations, usage, etc.)
3. From that request's headers, copy two values:
   - `authorization: Bearer eyJhbGci…` → everything after `Bearer ` is your **token**
   - `oai-device-id: …` → your **device ID**

**Set them:** paste both on the Tokens page under **Codex → Bearer token**
and **Device ID**.

> **Shortcut:** right-click the `backend-api` request → **Copy → Copy as
> cURL** and hand the whole curl to Claude Code — it pulls both values and
> pushes them to the device for you (`POST /api/settings`).

> **Note:** the Codex bearer token is short-lived (~24 h). When the Codex
> channel shows a `401`, repeat these steps with a fresh request. The Claude
> `sessionKey` lasts much longer but eventually needs the same refresh.

## Disclaimer — personal & educational use only

glimmer is shared for **personal experimentation and educational
purposes**.

It reads your own Claude and Codex usage by sending **your own
credentials** (a `sessionKey` cookie for claude.ai, a Bearer token for
chatgpt.com) to internal endpoints those services use to power their
web UIs. **These endpoints are undocumented and are not part of either
provider's public API.** They can change or be removed without notice,
and accessing them programmatically may be inconsistent with
Anthropic's or OpenAI's Terms of Service depending on interpretation.

By installing or modifying this firmware you accept full responsibility
for:

- Your own compliance with the relevant Terms of Service.
- Anything that happens on your own devices, accounts, or network.
- Securing your credentials — they are stored on the device's LittleFS
  partition in plaintext (the device runs on your home Wi-Fi behind
  your router).

This is a hobbyist project shared as-is, with no warranty, no support
guarantee, and no claim of fitness for any particular purpose. **Do not
redistribute as a commercial product. Do not use this to access accounts
that are not yours.**

## License

MIT. See [LICENSE](./LICENSE). The MIT license governs the code in
this repository; it does **not** waive any obligations you may have
under third-party Terms of Service (Anthropic, OpenAI, etc.). See the
Disclaimer above.

## Acknowledgments

- Inspired by [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) by Hermann Björgvin.
- GeekMagic SmallTV-Ultra — the hardware.
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver.
- [VT323](https://fonts.google.com/specimen/VT323),
  [Silkscreen](https://fonts.google.com/specimen/Silkscreen),
  [DM Mono](https://fonts.google.com/specimen/DM+Mono),
  [Pixelify Sans](https://fonts.google.com/specimen/Pixelify+Sans)
  — typography (all OFL).

---

<p align="center">
  <sub>Designed and built with <a href="https://claude.com/code">Claude Code</a>.</sub>
</p>
