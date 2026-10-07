---
name: flash-device
description: Use this skill to flash a GeekMagic SmallTV-Ultra device with glimmer firmware. Walks the user end-to-end including detecting device state (brand-new stock vs already-glimmer), guiding through Wi-Fi AP handoff, verifying connectivity at every step, downloading or building images, OTA-flashing firmware and filesystem, and keeping the device's config across a filesystem flash. Trigger phrases — "flash my SmallTV", "flash glimmer", "install glimmer", "set up glimmer device", "reflash glimmer", "update glimmer".
---

# Flash a glimmer device — interactive walkthrough

You are walking a user through flashing glimmer onto their GeekMagic
SmallTV-Ultra. **Be conversational and verify each step before moving
on.** Many steps need the user to physically do something (plug in the
device, join a Wi-Fi AP) — wait for confirmation, don't proceed blindly.

Facts this relies on:

- OTA endpoint: `POST http://<ip>/update`, form field exactly `firmware` or
  `filesystem`. A firmware flash keeps the filesystem; a filesystem flash
  replaces everything on it (`/config.json`, usage history, fonts, web UI).
- Prebuilt images (rolling `latest` release, rebuilt on every push to `main`):
  `https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin`
  and `.../latest/littlefs.bin`. Pinned versions: `.../download/vX.Y.Z/…`.
- `GET /api/state` → `fw`, `wifi` (`connected`/`ap`), `ip`,
  `claude_configured`, `codex_configured`, `weather_configured`, `heap`, …
- `GET /api/export` → the raw `/config.json` **including secrets** (Wi-Fi
  password, keys). Keep exports in a scratch dir and delete them afterwards.

## Phase 0 — orient

Ask the user:

1. **Brand-new (stock GeekMagic firmware) or already running glimmer?**
   Stock shows the GeekMagic weather-clock UI; glimmer shows the GLIMMER
   splash on boot.
2. **Are they next to the device?** Joining an AP needs physical proximity.
3. **Which OS is their laptop?** (macOS / Linux / Windows.)

Branch:
- **Brand-new device** → Phase A.
- **Already running glimmer** → Phase B.
- **Glimmer, but stuck on `glimmer-setup` / new router** → Phase C.

## Phase A — brand-new device (stock firmware)

Do the first flash over the **home LAN**, not over the stock `GIFTV` AP: TCP
backpressure on the slow AP wedges curl mid-upload.

### A.1 Prerequisites

```bash
curl --version | head -1
pio --version     # only needed to build from source
```

Downloads need only `curl`. If the user wants to build from source and `pio`
is missing: macOS `brew install platformio`, otherwise `pipx install platformio`.

### A.2 Get the stock firmware onto home Wi-Fi

**Stop and instruct the user:**

> Power on the device. If it may remember an old network, factory-reset it
> by power-cycling 3 times (plug in, see the progress bar, unplug, repeat);
> on the third power-on it boots into AP mode.
>
> **Join the Wi-Fi network `GIFTV`** (open). Ignore "no internet".
> Tell me when you're connected.

Verify:

```bash
curl -s --max-time 5 -o /dev/null -w "%{http_code}\n" http://192.168.4.1/
```

`200`/`301` = good. Timeout → they're probably not on `GIFTV`.

### A.3 Configure stock firmware to join home Wi-Fi

> Open `http://192.168.4.1/`, click **Scan**, pick your home Wi-Fi (2.4 GHz
> only), enter the password, Save. The device reboots. Then switch your
> laptop back to your home Wi-Fi.

### A.4 Find the device on the LAN

The stock firmware shows its IP in small text at the bottom of the screen —
ask the user first. Otherwise:

```bash
# macOS — refresh ARP cache, look for Espressif OUIs
for i in $(seq 1 254); do ping -c 1 -W 100 -t 1 192.168.<subnet>.$i &>/dev/null & done; wait
arp -an | grep -iE "78:21:84|24:6f:28|30:ae:a4|94:b9:7e|cc:50:e3"
# Linux
arp -a | grep -iE "espressif|78:21:84|24:6f:28"   # or: nmap -sn 192.168.<subnet>.0/24
```

Confirm it's the SmallTV (stock serves `/city.json` with a `"loc":` key):

```bash
curl -s --max-time 3 http://<ip>/city.json | head -c 100
```

### A.5 Get the images

**Default — prebuilt** (unless the user wants uncommitted local changes):

```bash
W=$(mktemp -d) && cd "$W"
curl -L -f -o littlefs.bin https://github.com/Avinava/glimmer/releases/download/latest/littlefs.bin
curl -L -f -o firmware.bin https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin
ls -la firmware.bin littlefs.bin
```

**Fallback — build** (from the repo root; both must print `[SUCCESS]`):

```bash
pio run -e nodemcuv2 -t buildfs      # .pio/build/nodemcuv2/littlefs.bin
pio run -e nodemcuv2                 # .pio/build/nodemcuv2/firmware.bin
```

### A.6 First flash — filesystem, then firmware

Filesystem **first**: it carries the fonts and web UI glimmer boots into.

```bash
DEVICE_IP=<from A.4>
curl -F "filesystem=@littlefs.bin" http://$DEVICE_IP/update
curl -F "firmware=@firmware.bin"   http://$DEVICE_IP/update
```

If the second upload can't reach `$DEVICE_IP` (the device rebooted), tell
the user:

> The device should now broadcast **`glimmer-setup`** (open). Join it and
> tell me when connected.

Then verify and flash firmware through the AP:

```bash
curl -s --max-time 5 http://192.168.4.1/api/state     # "wifi":"ap"
curl -F "firmware=@firmware.bin" http://192.168.4.1/update
```

### A.7 First-time setup

> On `glimmer-setup`, open `http://192.168.4.1/` and enter your home Wi-Fi
> (2.4 GHz) — Save & Restart. Switch back to your home Wi-Fi, then open
> `http://glimmer.local/` (or the IP shown on screen) and add your Claude
> `sessionKey` / Codex token, weather location and screens.

Verify:

```bash
curl -s http://glimmer.local/api/state     # "wifi":"connected", "fw":"<version>"
```

**Done with Phase A.**

## Phase B — already-glimmer device (update)

### B.1 Find it

```bash
curl -s --max-time 3 http://glimmer.local/api/state
```

Note `fw` and the `*_configured` flags. No mDNS → ask for the IP.

### B.2 Firmware-only or firmware + filesystem?

Firmware + filesystem is needed when anything under `data/` changed (fonts,
web UI) between the device's `fw` and the target. If unsure, check
`git log <device-version-commit>..HEAD -- data/` or just do B.4.

### B.3 Firmware only (config untouched)

```bash
W=$(mktemp -d) && cd "$W"
curl -L -f -o firmware.bin https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin
curl -F "firmware=@firmware.bin" http://<ip>/update
for i in $(seq 1 40); do curl -sf --max-time 3 http://<ip>/api/state && break; done
# Local build instead: pio run -e nodemcuv2, then .pio/build/nodemcuv2/firmware.bin
```

Confirm the new `fw`. Done.

### B.4 Firmware + filesystem — preferred: bake the config in (no AP)

The filesystem image is built with the device's own `config.json` inside, so
the device reboots straight onto its Wi-Fi with every setting intact. Needs
the repo checked out and PlatformIO (the FS image must be built locally).

```bash
cd <repo-root>
IP=<device-ip>; S=$(mktemp -d)                 # scratch: will hold secrets
curl -sf -o "$S/cfg.json" http://$IP/api/export
jq -e '.wifi_ssid' "$S/cfg.json"
```

The export must be the raw config (snake_case keys like `wifi_ssid`,
`claude_key`). camelCase keys with `"***"` mean the device had no saved
config — stop and use B.5.

```bash
cp -R data "$S/fs" && cp "$S/cfg.json" "$S/fs/config.json"
pio run -e nodemcuv2                                    # or download firmware.bin
PLATFORMIO_DATA_DIR="$S/fs" pio run -e nodemcuv2 -t buildfs

curl -F "firmware=@.pio/build/nodemcuv2/firmware.bin" http://$IP/update
for i in $(seq 1 40); do curl -sf --max-time 3 http://$IP/api/state && break; done
curl -F "filesystem=@.pio/build/nodemcuv2/littlefs.bin" http://$IP/update
for i in $(seq 1 40); do curl -sf --max-time 3 http://$IP/api/state && break; done
```

Verify `"wifi":"connected"`, the new `fw`, and the `*_configured` flags as
before. Then **clean up** — the scratch dir and the built image both contain
secrets:

```bash
trash "$S"                       # or rm -rf "$S"
pio run -e nodemcuv2 -t buildfs  # overwrite littlefs.bin with the normal image
```

Usage history (`/usage.bin`) and the weather cache are not preserved; they
refill on their own.

### B.5 Firmware + filesystem — fallback: restore through the setup AP

Use when the FS can't be built locally (prebuilt `littlefs.bin` only).

```bash
S=$(mktemp -d)                                       # scratch: holds secrets
curl -s -o "$S/cfg.json" http://<ip>/api/export
curl -F "firmware=@firmware.bin"   http://<ip>/update
curl -F "filesystem=@littlefs.bin" http://<ip>/update   # config gone → AP
```

> The device is rebooting into **`glimmer-setup`** (open AP). Join it and
> tell me when connected.

```bash
curl -s http://192.168.4.1/api/state     # "wifi":"ap", *_configured false
curl -X POST -H 'Content-Type: application/json' \
     --data-binary @"$S/cfg.json" http://192.168.4.1/api/import
# {"ok":true,"restart":true} — reboots onto home Wi-Fi
```

> Switch back to your home Wi-Fi.

Verify `/api/state` as in B.4, then `trash "$S"`.

## Phase C — device stuck on `glimmer-setup`

A running glimmer that loses Wi-Fi keeps retrying in place; it only brings up
`glimmer-setup` at **boot** when two 30 s connect attempts fail. In AP mode
it retries the saved network every 3 minutes, so a router that was simply
late recovers on its own.

If the network really changed: have the user join `glimmer-setup`, open
`http://192.168.4.1/` and enter the new Wi-Fi, or `POST /api/import` a backup
with the new credentials.

## Failure modes

- **`curl: (28)` / connection reset mid-OTA** → device is rebooting or the
  laptop is on the wrong network. Wait for `/api/state`, retry.
- **Tiny 5×7 text everywhere** → fonts missing: the filesystem wasn't
  flashed. Flash it (B.4/B.5).
- **`Auth -1` / `Auth -2`** → BearSSL handshake failed under heap pressure,
  not a bad key. Check `/api/state` `maxblk` and `heap_refusals`.
- **Setup AP doesn't appear** → the device may still be in its boot connect
  attempts (~1 min). Wait, or power-cycle.

Deep diagnosis needs `pio device monitor -b 115200` over a USB-TTL adapter on
the board's debug pads (USB-C has no data lines) — most users skip this.

## Who does what

| Action | Who |
|---|---|
| Confirm OS / device state | ask user |
| Plug in / power-cycle device | user |
| Join `GIFTV` or `glimmer-setup` | user |
| Configure Wi-Fi in the stock UI | user |
| `curl` checks, downloads, builds, `/update` uploads | you |
| Export / bake / restore config, delete scratch copies | you |
| Enter keys and preferences in the web UI | user |

Never run a step whose physical precondition (which Wi-Fi the laptop is on,
device powered) hasn't been confirmed — most failed ESP8266 OTAs are that.
