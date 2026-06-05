# Flashing glimmer onto a GeekMagic SmallTV-Ultra

This flashes the firmware over Wi-Fi — no case opening, no soldering, no
USB-TTL adapter. The trick is to do the first flash over your **home LAN**,
not over the stock device's AP, because TCP backpressure on the
ESP8266's tiny buffers wedges curl mid-upload over the slow AP.

## TL;DR

1. Get the stock firmware onto your home Wi-Fi (one-time).
2. Find the device's home-LAN IP.
3. Download the prebuilt images (no toolchain needed):
   `curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/littlefs.bin`
   `curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin`
4. `curl -F "filesystem=@littlefs.bin" http://<device-ip>/update` (filesystem **first**)
5. `curl -F "firmware=@firmware.bin"   http://<device-ip>/update`
6. Device boots into glimmer's setup AP. Connect to it once to enter
   your real Wi-Fi credentials.

That's it. (Prefer to build from source? See Step 3, Option B.)

---

## Hardware reference

| Item | Value |
|---|---|
| MCU | ESP8266 |
| Flash | 4 MB |
| Stock layout | `eagle.flash.4m3m.ld` (1 MB sketch, 3 MB LittleFS) |
| glimmer layout | `eagle.flash.4m1m.ld` (3 MB sketch, 1 MB LittleFS) |
| Display | 240×240 ST7789V IPS TFT |
| Display **color inversion** | **REQUIRED:** `tft.invertDisplay(true)` |
| Display pins | MOSI=GPIO13, SCLK=GPIO14, DC=GPIO0, RST=GPIO2, CS=floating/-1 |
| **Backlight** | GPIO5, **ACTIVE-LOW PWM**: `analogWrite(5, 0)` = full bright; `1023` = off |
| USB-C port | **Power only** — no data lines wired to MCU |
| OTA endpoint | `POST /update`, form field `firmware` / `filesystem`, no auth |

---

## Step 1 — Bring the SmallTV onto your home Wi-Fi (one-time, stock firmware)

If the device is brand-new it will boot into stock-firmware AP mode.
The stock AP is named **`GIFTV`** (open, no password).

If your device has stale Wi-Fi credentials, factory-reset by power-cycling
three times: plug in, watch the progress bar start, unplug immediately,
repeat. On the third boot the stock firmware enters AP mode with cleared
settings.

1. Connect a phone or laptop to **`GIFTV`** (your captive portal may
   complain about "no internet" — ignore).
2. Open `http://192.168.4.1/` in a browser.
3. Click **Scan**, pick your home Wi-Fi (2.4 GHz only on stock), enter
   the password, save. Device reboots and joins your network.
4. Confirm the device shows its home-LAN IP on screen.

> Note: the stock firmware has an information leak —
> `GET http://192.168.4.1/config.json` returns saved Wi-Fi creds in
> plaintext. Be aware. (glimmer fixes this — its `/api/export` is
> standard JSON without leaking passwords in `GET` to unauth clients;
> wifi password is masked.)

---

## Step 2 — Find the device's IP

From macOS:

```bash
# Refresh ARP cache, then look for the SmallTV
for i in $(seq 1 254); do ping -c 1 -W 100 -t 1 192.168.0.$i &>/dev/null & done; wait
arp -an | grep -iE "78:21:84|24:6f:28|30:ae:a4|94:b9:7e|cc:50:e3"
```

Or use the screen — stock firmware shows the IP in small text at the bottom.

Linux:

```bash
arp -a | grep -iE "espressif|78:21:84|24:6f:28"
# or
nmap -sn 192.168.0.0/24
```

---

## Step 3 — Get the glimmer images

### Option A — download the prebuilt binaries (recommended, no toolchain)

CI builds every push to `main` and publishes the images to the rolling
[`latest`](https://github.com/Avinava/glimmer/releases/tag/latest) release.
Grab them into your working directory:

```bash
curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/littlefs.bin
curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin
```

For a pinned version instead of the rolling latest, use a `v*` tag's assets:
`https://github.com/Avinava/glimmer/releases/download/v0.20.2/firmware.bin`.

### Option B — build from source (for developers)

```bash
cd <repo>
pio run -e nodemcuv2              # builds firmware.bin
pio run -e nodemcuv2 -t buildfs   # builds littlefs.bin (fonts + web UI)
```

Both artifacts land in `.pio/build/nodemcuv2/`. If PlatformIO isn't
installed: `brew install platformio` (macOS) or `pip install platformio`.

---

## Step 4 — Flash glimmer

**Order matters.** Flash filesystem FIRST so the device boots straight
into glimmer's setup mode (where it expects glimmer's fonts on FS).

The commands below assume **Option A** (binaries in your current directory).
For **Option B**, point the paths at `.pio/build/nodemcuv2/` instead.

```bash
DEVICE_IP=<your device's home-LAN IP>

curl -F "filesystem=@littlefs.bin" http://$DEVICE_IP/update
# wait ~10 s for reboot, device drops to AP mode

curl -F "firmware=@firmware.bin"   http://$DEVICE_IP/update
# OR (after device is in AP mode): http://192.168.4.1/update
```

After both flashes complete, the device boots into glimmer.

---

## Step 5 — First-time setup (glimmer's AP)

After the first flash the device boots into AP mode:

- **AP SSID**: `glimmer-setup` (open, no password)
- **Setup page**: `http://192.168.4.1/`

Steps:

1. Connect your phone/laptop to `glimmer-setup`.
2. Open `http://192.168.4.1/` in a browser.
3. Wi-Fi tab — enter your home Wi-Fi SSID + password. Click **Save & Restart**.
4. Wait ~20 s. The device reboots and joins your home Wi-Fi.
5. Find it again (`arp -an` or `http://glimmer.local/` via mDNS).
6. Optional: enter Claude / Codex tokens, weather lat/lon, channel toggles
   on the respective tabs.

---

## Re-flashing (any subsequent update)

Once glimmer is on the device (download the latest images first, or use
your local `.pio/build/nodemcuv2/` build):

```bash
# Latest CI build (or skip if building locally):
curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/firmware.bin
curl -L -O https://github.com/Avinava/glimmer/releases/download/latest/littlefs.bin

# Optional: back up your config first (uploadfs wipes /config.json)
curl -s -o /tmp/glimmer-config-backup.json http://<device-ip>/api/export

# Firmware-only flash (preserves config):
curl -F "firmware=@firmware.bin" http://<device-ip>/update

# Full flash (firmware + new fonts/web UI):
curl -F "firmware=@firmware.bin"   http://<device-ip>/update
curl -F "filesystem=@littlefs.bin" http://<device-ip>/update
# (config wiped — restore via setup AP and POST the backup to /api/import)
```

You can also use the web UI's "Reboot" button (Device page) instead of
power-cycling.

---

## Acknowledgments to the original community work

The "OTA-only first flash" technique was a community discovery. The
widely-cited advice that you must use UART for the first flash is wrong
— it's wrong because everyone tried OTA over the stock device's slow
GIFTV AP, where TCP backpressure on the ESP8266's tiny buffers wedges
curl mid-upload. Over your **home LAN**, the same OTA finishes cleanly.
