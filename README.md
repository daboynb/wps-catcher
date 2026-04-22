# wps-catcher

> **⚠️ DEVELOPMENT STATUS:** This project is currently under active development and is not yet considered stable.

ESP32 firmware that turns a $3 board into a pocket WPS attack tool. Boots into a SoftAP + captive portal, serves a live web UI over WebSocket, and drives WPS PIN attacks against nearby access points from the on-chip Wi-Fi stack.

> **wps-catcher is for authorized security testing, education, and research on networks you own or have explicit written permission to assess. Running WPS attacks against third-party networks is illegal in most jurisdictions.**

## Why on an ESP32

Reaver / Bully / `pixiewps` on a laptop with a monitor-mode USB adapter is the canonical setup. It works, but it is bulky, noisy on the air, fragile (monitor mode breaks every kernel update), and overkill — WPS is a short EAPOL sequence that does not need a full Linux box.

The ESP32 already has first-class WPS support (`esp_wps.h`), an HTTP server (`esp_http_server.h`), WebSocket, and NVS storage. It boots under a second, runs off USB power, and fits in a matchbox.

## What the firmware does

On flash, the device comes up as a dual-role node:

1. **SoftAP** broadcasting `wps-catcher` on `192.168.4.1`.
2. **Captive DNS** that NXDOMAINs everything except `192.168.4.1`, so every phone's captive-portal detector lands on the UI automatically.
3. **Web UI** (SPIFFS-served) with target selection, live WPS attack, and saved-networks screens. WebSocket pushes every phase change and every attempted PIN.
4. **STA mode** on the second radio slice for scanning and the WPS handshake itself.

From power-on to first PIN attempt is ~7 s.

## Supported targets

| Board | CPU | Notes |
|-------|-----|-------|
| `esp32` | 240 MHz | 2 MB app / 1 MB SPIFFS |
| `esp32s3` | 240 MHz | Native USB |
| `esp32c3` | 160 MHz | RISC-V, lowest power |

Toolchain: ESP-IDF via PlatformIO (`platformio.ini`).

## Build + flash

```bash
# Install PlatformIO, then:
pio run -e esp32 -t upload
pio run -e esp32 -t uploadfs    # flash SPIFFS web assets
pio device monitor
```

Swap `esp32` for `esp32s3` or `esp32c3` per board.

## Repository layout

```
wps-catcher/
├── src/
│   ├── main.c            # boot: NVS + SPIFFS + WiFi + DNS + HTTP server
│   ├── wifi_manager.c    # SoftAP + STA init
│   ├── wifi_wps.c        # WPS state machine, status pusher, STA-config snapshot
│   ├── wifi_saved.c      # NVS-backed saved networks
│   ├── server.c          # esp_http_server + WebSocket
│   ├── server_api.c      # HTTP/WS handlers
│   ├── dns.c             # captive-portal DNS responder
│   └── config.c          # config.json loader + NVS helpers
├── include/              # Public headers
├── data/                 # SPIFFS web UI (HTML/CSS/JS)
├── partition.csv         # NVS + app + SPIFFS layout
└── sdkconfig.*           # Per-board sdkconfig
```

## Footprint

- Firmware: ~1.6 MB
- RAM: ~40 KB steady state
- Current draw: ~90 mA on ESP32-C3
