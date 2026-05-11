#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "sdkconfig.h"

/* ============================================================
 * Per-board pinouts and feature flags.
 * ============================================================ */

#if CONFIG_PROBE_BOARD_WESP32
/* Silicognition wESP32 — LAN8720, ext 50 MHz osc into GPIO0, no PHY pwr-en. */
#define PROBE_BOARD_NAME            "wesp32"
#define PROBE_HOSTNAME_DEFAULT      "brainslug"
#define PROBE_MDNS_INSTANCE         "Brainslug (wESP32)"

#define WESP_ETH_PHY_ADDR           0
#define WESP_ETH_MDC_GPIO           16
#define WESP_ETH_MDIO_GPIO          17
#define WESP_ETH_PHY_RST_GPIO       (-1)

/* UART defaults — avoid RMII pins (0,16-19,21-23,25-27) and flash (6-11). */
#define UART1_DEFAULT_TX_GPIO       33
#define UART1_DEFAULT_RX_GPIO       32
#define UART2_DEFAULT_TX_GPIO       13
#define UART2_DEFAULT_RX_GPIO       14

#elif CONFIG_PROBE_BOARD_S3_POE_ETH_CAM
/* Waveshare ESP32-S3-POE-ETH-CAM-KIT — W5500 over SPI + OV2640 + PoE.
 * Pin numbers come from Waveshare's reference docs; verify against your
 * board revision before flashing.
 */
#define PROBE_BOARD_NAME            "s3-poe-eth-cam"
#define PROBE_HOSTNAME_DEFAULT      "brainslug"
#define PROBE_MDNS_INSTANCE         "Brainslug (S3 PoE-Cam)"

/* W5500 (SPI2 host) — from Waveshare wiki ESP32-S3-ETH. */
#define W5500_SPI_HOST              1   /* SPI2_HOST */
#define W5500_SPI_CLOCK_MHZ         36
#define W5500_SPI_MOSI_GPIO         11
#define W5500_SPI_MISO_GPIO         12
#define W5500_SPI_SCLK_GPIO         13
#define W5500_SPI_CS_GPIO           14
#define W5500_INT_GPIO              10
#define W5500_RST_GPIO              9
#define W5500_PHY_ADDR              1

/* OV2640/OV5640 camera (DVP) — from Waveshare wiki ESP32-S3-ETH. */
#define CAM_PIN_PWDN                (-1)
#define CAM_PIN_RESET               (-1)
#define CAM_PIN_XCLK                3
#define CAM_PIN_SIOD                48   /* SDA */
#define CAM_PIN_SIOC                47   /* SCL */
#define CAM_PIN_D7                  18
#define CAM_PIN_D6                  15
#define CAM_PIN_D5                  38
#define CAM_PIN_D4                  40
#define CAM_PIN_D3                  42
#define CAM_PIN_D2                  46
#define CAM_PIN_D1                  45
#define CAM_PIN_D0                  41
#define CAM_PIN_VSYNC               1
#define CAM_PIN_HREF                2
#define CAM_PIN_PCLK                39

/* Active-low power enable for the OV camera daughterboard. Drive LOW to
 * power the sensor (Waveshare schematic uses a P-FET high-side switch). */
#define CAM_PIN_PWR_EN              8

/* TF card slot SPI3 (CS=4, MISO=5, MOSI=6, SCK=7) — keep these reserved. */

/* UART defaults — must avoid W5500 (9-14), camera (1-3,15,18,38-48 minus 43/44),
 * SD (4-7), USB JTAG (19/20), and N16R8 octal PSRAM/flash (26-37).
 * That leaves a small free set: 0(strap), 8, 16, 17, 21, 43, 44.
 * Use 43/44 (TX/RX of the broken-out USB-serial header) and 17/16. */
#define UART1_DEFAULT_TX_GPIO       43
#define UART1_DEFAULT_RX_GPIO       44
#define UART2_DEFAULT_TX_GPIO       17
#define UART2_DEFAULT_RX_GPIO       16

#else
#error "No PROBE_BOARD_* selected. Run 'idf.py menuconfig' -> Probe board."
#endif

/* ============================================================
 * UART bridge
 * ============================================================ */
#define UART_BRIDGE_PORTS           2          /* UART1 + UART2; UART0 = console */
#define UART_RX_RING_BYTES          8192
#define UART_DEFAULT_BAUD           115200

typedef struct {
    int baud, data, parity, stop, tx, rx;
} uart_cfg_t;

esp_err_t uart_bridge_init_all(void);
esp_err_t uart_bridge_reconfigure(int port, const uart_cfg_t *cfg);
int       uart_bridge_write(int port, const uint8_t *buf, size_t len);
int       uart_bridge_read(int port, uint8_t *buf, size_t max_len, uint32_t timeout_ms);
esp_err_t uart_bridge_get_config(int port, uart_cfg_t *out);
bool      uart_port_valid(int port);
void      uart_bridge_set_ws_sink(int port, httpd_handle_t srv, int fd);
esp_err_t uart_bridge_break(int port, uint32_t ms);
esp_err_t uart_bridge_set_invert(int port, int rx_inv, int tx_inv);

/* ============================================================
 * Net config
 * ============================================================ */
typedef enum { NET_DHCP = 0, NET_STATIC = 1 } net_mode_t;
typedef struct {
    net_mode_t mode;
    char ip[16], netmask[16], gw[16], dns[16];
    char hostname[32];
} net_cfg_t;

esp_err_t net_cfg_load(net_cfg_t *out);
esp_err_t net_cfg_save(const net_cfg_t *cfg);
void      net_cfg_apply(esp_netif_t *netif, const net_cfg_t *cfg);

/* ============================================================
 * Eth init (per-board impl in eth_init.c)
 * ============================================================ */
esp_netif_t *probe_eth_start(const net_cfg_t *netcfg);

/* True if a GPIO is reserved by board peripherals (eth/cam) and must not
 * be exposed via /gpio. Implemented per-board in eth_init.c. */
bool probe_pin_reserved(int gpio);

/* ============================================================
 * HTTP server
 * ============================================================ */
esp_err_t http_api_start(void);

/* ============================================================
 * Camera (only when CONFIG_PROBE_CAMERA)
 * ============================================================ */
#if CONFIG_PROBE_CAMERA
esp_err_t probe_camera_init(void);
void      probe_camera_register_routes(httpd_handle_t srv);
#endif

/* ============================================================
 * OTA confirm
 * ============================================================ */
void      ota_arm_auto_confirm(void);
