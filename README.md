# Brainslug

Network-attached debug probe firmware. Exposes an HTTP + WebSocket API so an
agent can wiggle GPIOs, transceive two independent UARTs at runtime-configurable
baud, push new firmware over Ethernet, and (on camera-equipped boards) grab
JPEG snapshots / MJPEG streams.

## Supported boards

Pick at build time with `idf.py set-target` — Kconfig auto-selects the right
`PROBE_BOARD_*` from `sdkconfig.defaults.<target>`.

| Target        | Board                                    | Eth        | Camera | mDNS                |
| ------------- | ---------------------------------------- | ---------- | ------ | ------------------- |
| `esp32`       | Silicognition wESP32 (LAN8720, PoE)      | int. EMAC  | —      | `wesp-probe.local`  |
| `esp32s3`     | Waveshare ESP32-S3-POE-ETH-CAM-KIT       | W5500 SPI  | OV2640 | `s3-probe.local`    |

## Build & flash

ESP-IDF v5.3.x. The LXC at `tc8` already has it set up at `~/esp-idf`.

```sh
. $IDF_PATH/export.sh
# wESP32:
idf.py set-target esp32 && idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
# Waveshare S3-POE-ETH-CAM:
idf.py set-target esp32s3 && idf.py build && idf.py -p /dev/ttyACM0 flash monitor
```

## Hardware pinouts

### wESP32 (`esp32` target)
| Function       | GPIO          |
| -------------- | ------------- |
| RMII REF_CLK   | 0  (input from external 50 MHz oscillator) |
| RMII MDC / MDIO| 16 / 17 |
| RMII data      | 18, 19, 21, 22, 23, 25, 26, 27 |
| UART1 TX / RX (default) | 33 / 32 |
| UART2 TX / RX (default) | 13 / 14 |

### ESP32-S3-POE-ETH-CAM (`esp32s3` target)
W5500 SPI: MOSI=11, MISO=13, SCLK=12, CS=14, INT=10, RST=9.
OV2640: XCLK=15, SIOD=4, SIOC=5; D0..D7, VSYNC, HREF, PCLK on the camera
header. UART1 default 43/44 (USB-serial header), UART2 default 1/2.
**Pin numbers are board-revision sensitive — verify against your schematic
and adjust `main/probe.h` if the link doesn't come up.**

UART pins are runtime-overridable. Anything not flagged by
`probe_pin_reserved()` (see `eth_init.c`) is fair game via `/gpio`.

## HTTP API

| Method | Path | Body / Query | Returns |
| ------ | ---- | ------------ | ------- |
| GET  | `/info`              | —                        | app/version/idf/running partition |
| GET  | `/gpio?pin=N`        | —                        | `{pin,level}` |
| POST | `/gpio`              | `{"pin":N,"mode":"in\|out\|in_pu\|in_pd\|od","level":0\|1}` | `{pin,level}` |
| GET  | `/uart/{N}/config`   | —                        | current config |
| POST | `/uart/{N}/config`   | `{"baud":..,"data":..,"parity":..,"stop":..,"tx_gpio":..,"rx_gpio":..}` | new config |
| POST | `/uart/{N}/write`    | raw bytes                | `{port,written}` |
| GET  | `/uart/{N}/read`     | `?max=1024&timeout_ms=100` | raw bytes; `X-UART-Bytes` header |
| WS   | `/uart/{N}/ws`       | binary frames in/out     | full-duplex stream |
| GET  | `/net`               | —                        | mode, hostname, configured + current IP |
| POST | `/net`               | `{"mode":"dhcp\|static","ip":..,"netmask":..,"gw":..,"dns":..,"hostname":..}` | saved (reboot to apply) |
| POST | `/ota`               | raw firmware `.bin`      | reboots into new image |
| POST | `/reboot`            | —                        | reboots |
| GET  | `/camera/info`       | —                        | sensor PID, framesize, quality (S3 only) |
| GET  | `/camera/snapshot`   | —                        | one JPEG frame (S3 only) |
| GET  | `/camera/stream`     | —                        | MJPEG `multipart/x-mixed-replace` (S3 only) |

`{N}` is `1` or `2`. UART config is persisted to NVS — survives reboots.
`parity`: 0=none, 1=odd, 2=even. All `/uart` config fields are optional.

### Examples

```sh
# blink GPIO 5
curl -XPOST -d '{"pin":5,"mode":"out","level":1}' http://wesp-probe.local/gpio
sleep 0.5
curl -XPOST -d '{"pin":5,"level":0}'              http://wesp-probe.local/gpio

# UART1 to 921600 8N1 on different pins
curl -XPOST -H 'Content-Type: application/json' \
  -d '{"baud":921600,"tx_gpio":4,"rx_gpio":5}' http://wesp-probe.local/uart/1/config

# poll mode: write then read
printf 'AT\r\n' | curl --data-binary @- http://wesp-probe.local/uart/1/write
curl 'http://wesp-probe.local/uart/1/read?max=4096&timeout_ms=200' --output -

# stream mode: full-duplex over websocket (binary frames)
websocat ws://wesp-probe.local/uart/1/ws

# pin a static IP
curl -XPOST -d '{"mode":"static","ip":"192.168.10.42","netmask":"255.255.255.0","gw":"192.168.10.1","dns":"192.168.10.1","hostname":"probe-a"}' http://wesp-probe.local/net
curl -XPOST http://wesp-probe.local/reboot

# OTA update
idf.py build
curl -XPOST --data-binary @build/wesp_debug_probe.bin http://wesp-probe.local/ota
```

## Behavior notes

- **OTA rollback safety net:** new images boot in pending-verify state. After
  30 s of successful uptime the firmware self-marks-valid; if it crashes or
  is power-cycled before then, the bootloader reverts to the previous slot.
- **`/uart/N/ws` is full-duplex.** Client→slug binary frames are flushed to
  the UART TX FIFO; slug→client binary frames carry the UART RX bytes.
  One sink per port, *last-wins*: a new connection closes the previous one,
  so reconnects always succeed without waiting for the old fd to be reaped.
  While a sink is attached, polling `/uart/N/read` returns 0 bytes (the
  streamer drains the RX ring straight to the WS). For throughput-sensitive
  flows (catching u-boot, terminal echo) prefer WS — HTTP `POST /uart/N/write`
  has per-request handshake overhead that caps practical write rates around
  ~60–100/s, vs >1 k/s over WS.
- **Net config persistence:** `/net` saves to NVS, reboot to apply. Static
  mode requires `ip` + `netmask`; the rest are optional. Bad config → power
  cycle while holding GPIO 0 low to recover via USB and reflash.
- **No API auth.** Run on a trusted segment or wrap in a reverse proxy.
- **mDNS:** advertises `_http._tcp` on the configured hostname (default
  `wesp-probe`). Change via `/net`.

## WS2812 onboard RGB LED (s3-poe-eth-cam) — GPIO21

The Waveshare board's WS2812 powers up **full-white** and washes out the
camera; it latches from a serial frame, so GPIO-input/power-cycle can't
turn it off — only clocking it a {0,0,0} frame does. `main/led.c`
(led_strip/RMT) blanks it at boot (`led_init()` first thing in
`app_main`) and exposes runtime control:

    GET /led?r=&g=&b=     # 0-255 each; no args = off

Note: this `led_strip` (2.5.5 here) uses the **older API**
(`led_pixel_format`/`LED_PIXEL_FORMAT_GRB`, not
`color_component_format`). Build on tc8 (`/root/brainslug`,
`. /root/esp-idf/export.sh && idf.py build`), OTA via
`curl --data-binary @build/brainslug.bin http://192.168.10.95/ota`
(dual-slot; PoE port-4 cycle reverts a bad image).
