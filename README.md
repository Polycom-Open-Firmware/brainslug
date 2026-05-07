# wesp_debug_probe

Network-attached debug probe firmware for the
[Silicognition wESP32](https://hackaday.io/project/85389-wesp32-wired-esp32-with-ethernet-and-poe)
(PoE-powered ESP32 + LAN8720). Exposes an HTTP API so an agent can wiggle
GPIOs, transceive a UART stream at runtime-configurable baud, and push new
firmware over Ethernet.

## Build & flash (first time, USB)

Requires ESP-IDF v5.x.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The board pulls DHCP on boot. Watch the monitor for the assigned IP, then:

```sh
curl http://<ip>/info
```

## Hardware pinout assumed

| Function       | GPIO          |
| -------------- | ------------- |
| RMII REF_CLK   | 0 (input from external 50 MHz oscillator) |
| RMII MDC       | 16 |
| RMII MDIO      | 17 |
| RMII TX/RX/CRS | 18, 19, 21, 22, 23, 25, 26, 27 (fixed by EMAC) |
| UART1 TX (default) | 33 |
| UART1 RX (default) | 32 |

Pins 6–11 are flash, 16/17 are MDC/MDIO, the RMII set above is reserved.
Everything else is fair game via the GPIO API.

## HTTP API

| Method | Path             | Body / Query                                                                 | Returns |
| ------ | ---------------- | ---------------------------------------------------------------------------- | ------- |
| GET    | `/info`          | —                                                                            | app/version/idf |
| GET    | `/gpio?pin=N`    | —                                                                            | `{pin,level}` |
| POST   | `/gpio`          | `{"pin":N,"mode":"in\|out\|in_pu\|in_pd\|od","level":0\|1}`                  | `{pin,level}` |
| GET    | `/uart/config`   | —                                                                            | current config |
| POST   | `/uart/config`   | `{"baud":115200,"data":8,"parity":0,"stop":1,"tx_gpio":33,"rx_gpio":32}`     | new config |
| POST   | `/uart/write`    | raw bytes (Content-Type irrelevant)                                          | `{"written":N}` |
| GET    | `/uart/read`     | `?max=1024&timeout_ms=100`                                                   | raw bytes; `X-UART-Bytes` header |
| POST   | `/ota`           | raw firmware `.bin` (`build/wesp_debug_probe.bin`)                           | reboots into new image |
| POST   | `/reboot`        | —                                                                            | reboots |

`parity`: 0=none, 1=odd, 2=even. All fields in `/uart/config` are optional;
omitted fields keep their current value.

### Examples

```sh
# blink GPIO 5
curl -XPOST -d '{"pin":5,"mode":"out","level":1}' http://probe/gpio
sleep 0.5
curl -XPOST -d '{"pin":5,"level":0}'              http://probe/gpio

# switch UART to 921600 8N1 on different pins
curl -XPOST -H 'Content-Type: application/json' \
  -d '{"baud":921600,"tx_gpio":13,"rx_gpio":14}' http://probe/uart/config

# send "AT\r\n" then read whatever shows up in the next 200 ms
printf 'AT\r\n' | curl --data-binary @- http://probe/uart/write
curl 'http://probe/uart/read?max=4096&timeout_ms=200' --output -

# OTA update
idf.py build
curl -XPOST --data-binary @build/wesp_debug_probe.bin http://probe/ota
```

## Notes

- Two OTA slots are configured (1.5 MB each). Rollback is enabled; if the
  new image never sets itself valid via `esp_ota_mark_app_valid_cancel_rollback`
  on next boot, the bootloader reverts. Add that call once you trust an
  image — current firmware does not, so OTA is "always tentative" and a
  power cycle reverts. Acceptable for a probe; tighten if you redeploy unattended.
- No auth on the API. Run on a trusted segment or wrap in a reverse proxy.
- UART1 only. UART0 is left to the boot ROM/console.
