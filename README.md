# wesp_debug_probe

Network-attached debug probe firmware for the
[Silicognition wESP32](https://hackaday.io/project/85389-wesp32-wired-esp32-with-ethernet-and-poe)
(PoE-powered ESP32 + LAN8720). Exposes an HTTP + WebSocket API so an agent
can wiggle GPIOs, transceive two independent UARTs at runtime-configurable
baud, and push new firmware over Ethernet.

## Build & flash (first time, USB)

ESP-IDF v5.3.x. The LXC at `tc8` already has it set up at `~/esp-idf`.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

After the first flash the probe joins via DHCP and announces itself on
mDNS as `wesp-probe.local`.

## Hardware pinout

| Function       | GPIO          |
| -------------- | ------------- |
| RMII REF_CLK   | 0  (input from external 50 MHz oscillator) |
| RMII MDC / MDIO| 16 / 17 |
| RMII data      | 18, 19, 21, 22, 23, 25, 26, 27 |
| UART1 TX / RX (default) | 33 / 32 |
| UART2 TX / RX (default) | 13 / 14 |

UART pins are runtime-overridable. Pins 6–11 are flash; everything else not
in the RMII set is fair game via `/gpio`.

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
- **WS exclusivity per port:** while a UART has an active WebSocket sink,
  the polling `/uart/N/read` endpoint returns 0 bytes — the streamer
  drains the RX ring. Only one WS client per port.
- **Net config persistence:** `/net` saves to NVS, reboot to apply. Static
  mode requires `ip` + `netmask`; the rest are optional. Bad config → power
  cycle while holding GPIO 0 low to recover via USB and reflash.
- **No API auth.** Run on a trusted segment or wrap in a reverse proxy.
- **mDNS:** advertises `_http._tcp` on the configured hostname (default
  `wesp-probe`). Change via `/net`.
