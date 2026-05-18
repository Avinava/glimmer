---
name: flash-device
description: Use this skill to flash a GeekMagic SmallTV-Ultra device with glimmer firmware. Walks the user end-to-end including detecting device state (brand-new stock vs already-glimmer), guiding through Wi-Fi AP handoff, verifying connectivity at every step, building, OTA-flashing firmware and filesystem, and restoring config after a full flash. Trigger phrases — "flash my SmallTV", "flash glimmer", "install glimmer", "set up glimmer device", "reflash glimmer".
---

# Flash a glimmer device — interactive walkthrough

You are walking a user through flashing glimmer onto their GeekMagic
SmallTV-Ultra. **Be conversational and verify each step before moving
on.** Many steps require the user to physically do something
(plug in the device, join a Wi-Fi AP) — wait for confirmation, don't
proceed blindly.

## Phase 0 — orient

Start by asking the user:

1. **Is the device brand-new (running stock GeekMagic firmware), or
   already running glimmer?** If they're not sure, ask them to plug it
   in. Brand-new shows the stock weather-clock UI. Already-glimmer shows
   the GLIMMER splash with amber spark logo on boot.

2. **Are they in the same physical location as the device?** Some steps
   (joining its setup Wi-Fi AP) require physical proximity. Yes/no
   shapes the steps below.

3. **Which OS is their laptop?** (macOS / Linux / Windows). Network
   tools differ.

Branch based on answers:
- **Brand-new device** → Phase A (stock → home-Wi-Fi → first OTA).
- **Already running glimmer** → Phase B (OTA update only).
- **Already running glimmer but lost Wi-Fi** → Phase C (factory-reset
  recovery via setup AP).

## Phase A — brand-new device (stock firmware)

### A.1 Prerequisites

Run these checks and report results to the user:

```bash
pio --version       # PlatformIO CLI — install if missing
gh --version        # for repo work, optional
curl --version | head -1
```

If `pio` is missing, instruct user to install it:
- macOS: `brew install platformio`
- Linux/Windows: `pip install platformio`

### A.2 Get the device on the user's home Wi-Fi (stock firmware)

**Stop and instruct the user:**

> Power on the device. If it's been used before and you don't know if
> it remembers a Wi-Fi network, factory-reset by power-cycling 3 times
> (plug in, see the progress bar, unplug, repeat). On the third power-on
> it boots into AP mode.
>
> **Join the Wi-Fi network named `GIFTV` from your phone or laptop**
> (open, no password). Ignore the "no internet" warning.
> Then **let me know you're connected**.

Wait for confirmation. Then verify:

```bash
# Should respond with stock firmware HTML (or at least a connection):
curl -s --max-time 5 -o /dev/null -w "%{http_code}\n" http://192.168.4.1/
```

If it returns `200` or `301`, great. If timeout, ask user to confirm
they're on `GIFTV` (not their home Wi-Fi).

### A.3 Configure stock firmware to join home Wi-Fi

**Instruct the user:**

> Open `http://192.168.4.1/` in a browser. Click **Scan**, pick your
> home Wi-Fi (2.4 GHz only — stock can't see 5 GHz), enter the password,
> click Save. The device will reboot.

Wait for confirmation. Then ask them to switch their laptop **back to
their home Wi-Fi**.

### A.4 Find the device on the LAN

The device's home-LAN IP needs to be discovered. Run, depending on OS:

```bash
# macOS — refresh ARP cache, look for Espressif OUI
for i in $(seq 1 254); do ping -c 1 -W 100 -t 1 192.168.<subnet>.$i &>/dev/null & done; wait
arp -an | grep -iE "78:21:84|24:6f:28|30:ae:a4|94:b9:7e|cc:50:e3"

# Linux
arp -a | grep -iE "espressif|78:21:84|24:6f:28"
# or
nmap -sn 192.168.<subnet>.0/24
```

Ask user for `<subnet>` if you don't know it (usually `0` or `1`).

Verify the candidate IP is the SmallTV:

```bash
# Stock SmallTV serves /city.json
curl -s --max-time 3 http://<ip>/city.json | head -c 100
```

If you see JSON with a `"loc":` key, that's the device.

### A.5 Build glimmer

```bash
cd <repo-root>
pio run -e nodemcuv2 -t buildfs      # outputs .pio/build/nodemcuv2/littlefs.bin
pio run -e nodemcuv2                 # outputs .pio/build/nodemcuv2/firmware.bin
```

Both must print `[SUCCESS]`. If a build fails citing missing `tools/ttf/*.ttf`,
the user needs to run the `regenerate-fonts` skill first.

### A.6 First flash — filesystem then firmware

**Filesystem first** — it carries the VLW fonts. Without it, the
firmware renders fallback glyphs.

```bash
DEVICE_IP=<from A.4>

# 1. Filesystem
curl -F "filesystem=@.pio/build/nodemcuv2/littlefs.bin" http://$DEVICE_IP/update
# Device shows "OTA UPDATE" with a coral progress bar (well, after step 2
# completes — stock firmware doesn't show that screen yet). Wait ~15s.

# 2. Firmware
# After the FS flash the device reboots and may drop to its setup AP
# (because /config.json was wiped). It's no longer at $DEVICE_IP.
# We'll flash firmware via the setup AP next.
```

**Tell the user**:

> The device just rebooted. It should now broadcast a Wi-Fi AP called
> `glimmer-setup` (open). **Join `glimmer-setup` from your laptop and
> tell me when connected.**

Wait for confirmation. Then verify:

```bash
curl -s --max-time 5 http://192.168.4.1/api/state
```

Should return JSON with `"fw":"0.1.0"` (or whatever the repo's version
is) and `"wifi":"ap"`. If you see this, the FS flash worked.

```bash
# Now flash firmware via the setup AP
curl -F "firmware=@.pio/build/nodemcuv2/firmware.bin" http://192.168.4.1/update
```

### A.7 First-time setup

**Instruct user:**

> The device is still on `glimmer-setup`. Open `http://192.168.4.1/` in
> a browser. You'll see the glimmer web UI. Walk through these tabs:
>
> 1. **Wi-Fi** — enter your home Wi-Fi SSID + password. Click
>    "Save & Restart". The device reboots and joins your network.
> 2. Once back on your home Wi-Fi, find the device's new IP (mDNS:
>    `http://glimmer.local/`, or repeat A.4).
> 3. **Tokens** — paste Claude `sessionKey` cookie (from claude.ai →
>    DevTools → Application → Cookies → `sessionKey`). Optionally
>    Codex Bearer token. Save.
> 4. **Channels** — toggle which channels rotate.
> 5. **You** — optional name, birthday, weather lat/lon.

Verify the device is back:

```bash
# Mac (mDNS works):
curl -s http://glimmer.local/api/state
# Otherwise:
curl -s http://<new-device-ip>/api/state
```

Should return `"wifi":"connected"` and `"fw":"<version>"`. **Done with Phase A.**

## Phase B — already-glimmer device, OTA update

### B.1 Find the device

Ask the user for the IP, or try mDNS:

```bash
curl -s --max-time 3 http://glimmer.local/api/state
```

Confirm `claude_configured`, `codex_configured`, `weather_configured`
match what you expect. If the device responds, you have the IP.

### B.2 Back up config (always, before any FS upload)

```bash
curl -s -o /tmp/glimmer-config-backup.json http://<device-ip>/api/export
wc -c /tmp/glimmer-config-backup.json
# Should be a few hundred bytes to a few kB.
```

### B.3 Decide: firmware-only or firmware + filesystem?

**Firmware-only** is non-destructive — config stays. Suitable for code
changes that don't touch `data/`.

**Firmware + filesystem** wipes `/config.json` and reboots into setup AP.
Required if any file under `data/` (fonts, web UI) changed. Includes the
AP-handoff dance.

Ask the user explicitly: "Did you change anything under `data/`?"

### B.4 Firmware-only flash

```bash
pio run -e nodemcuv2
curl -F "firmware=@.pio/build/nodemcuv2/firmware.bin" http://<device-ip>/update
# Wait ~10 s for reboot, then:
curl -s http://<device-ip>/api/state    # confirm new fw version
```

Done.

### B.5 Full flash (firmware + filesystem)

```bash
pio run -e nodemcuv2 -t buildfs
pio run -e nodemcuv2

# Firmware first
curl -F "firmware=@.pio/build/nodemcuv2/firmware.bin" http://<device-ip>/update
# Wait ~10 s
curl -s http://<device-ip>/api/state    # confirm new fw

# Filesystem (wipes config, reboots to AP)
curl -F "filesystem=@.pio/build/nodemcuv2/filesystem.bin" http://<device-ip>/update || \
curl -F "filesystem=@.pio/build/nodemcuv2/littlefs.bin"  http://<device-ip>/update
```

**Tell the user:**

> The device is rebooting into `glimmer-setup` (open AP). **Join
> `glimmer-setup` from your laptop and confirm when connected.**

Wait for confirmation. Verify:

```bash
curl -s http://192.168.4.1/api/state
# Should show "wifi":"ap" and claude/codex/weather_configured all false.
```

Now restore the backed-up config:

```bash
curl -X POST -H 'Content-Type: application/json' \
     --data-binary @/tmp/glimmer-config-backup.json \
     http://192.168.4.1/api/import
# Returns {"ok":true,"restart":true} — device reboots and rejoins home Wi-Fi.
```

Wait ~20 s. Tell the user to **switch back to their home Wi-Fi**. Then
verify:

```bash
curl -s http://<device-ip>/api/state
# Should be "wifi":"connected" with all *_configured flags true again.
```

Done.

## Phase C — recovery (device lost Wi-Fi)

If glimmer is on the device but it can't join Wi-Fi (e.g., router
changed), it falls back to its `glimmer-setup` AP automatically after
~3.5 minutes of failed reconnect attempts.

Tell the user to wait for the AP, then proceed with B.5's recovery dance
(join AP → restore or re-enter credentials via `http://192.168.4.1/`).

## Failure modes to watch for

- **`curl: (28) Connection timed out`** mid-OTA → device is mid-reboot.
  Wait 15 s and retry.
- **Channels show tiny dotted text** → filesystem wasn't uploaded
  (fonts missing). Run `uploadfs` again.
- **Claude shows `auth -1` or `auth -2`** → BearSSL handshake failed.
  Heap pressure. Check `/api/state.heap` is ≥ 30 KB.
- **Setup AP not appearing** → device might still be retrying Wi-Fi.
  Wait 3-4 minutes, or unplug + replug to force AP mode.

For deep diagnosis: `pio device monitor -b 115200` (requires USB-TTL
adapter on the SmallTV-Ultra's debug pads, which are not exposed via
USB-C — most users skip this).

## What you do, what you ask the user to do

| Action | Who |
|---|---|
| Confirm OS / device state | ask user |
| Plug device in | user |
| Join `GIFTV` or `glimmer-setup` Wi-Fi | user |
| Run `curl` to verify device responds | you |
| Run `pio run` to build | you |
| Run `curl … /update` for OTA | you |
| Configure home Wi-Fi via stock UI | user |
| Enter tokens / channel preferences | user |
| Backup + restore config | you |

Don't run any "skip ahead" commands without the matching physical step
having been confirmed. ESP8266 OTA failures are mostly user-physical-state
mismatches — most often the laptop is on the wrong Wi-Fi.
