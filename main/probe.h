#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"

/* ---- WESP32 (Silicognition wESP32) Ethernet pinout ----
 * LAN8720, external 50 MHz oscillator drives RMII REF_CLK into GPIO0.
 * No PHY power-enable GPIO on this board.
 */
#define WESP_ETH_PHY_ADDR     0
#define WESP_ETH_MDC_GPIO     16
#define WESP_ETH_MDIO_GPIO    17
#define WESP_ETH_PHY_RST_GPIO -1

/* ---- UART bridge ---- */
#define UART_BRIDGE_PORTS     2          /* expose UART1 + UART2; UART0 = console */
#define UART_RX_RING_BYTES    8192
#define UART_DEFAULT_BAUD     115200
/* Defaults — pick pins that don't collide with ETH RMII (0,16-19,21-23,25-27)
 * or flash (6-11). All are runtime-overridable. */
#define UART1_DEFAULT_TX_GPIO 33
#define UART1_DEFAULT_RX_GPIO 32
#define UART2_DEFAULT_TX_GPIO 13
#define UART2_DEFAULT_RX_GPIO 14

typedef struct {
    int baud, data, parity, stop, tx, rx;
} uart_cfg_t;

esp_err_t uart_bridge_init_all(void);
esp_err_t uart_bridge_reconfigure(int port, const uart_cfg_t *cfg);
int       uart_bridge_write(int port, const uint8_t *buf, size_t len);
int       uart_bridge_read(int port, uint8_t *buf, size_t max_len, uint32_t timeout_ms);
esp_err_t uart_bridge_get_config(int port, uart_cfg_t *out);
bool      uart_port_valid(int port);

/* WS streaming: register an http server + ws fd to receive RX bytes from a port.
 * Pass fd<0 to clear. Only one streamer per port. */
void      uart_bridge_set_ws_sink(int port, httpd_handle_t srv, int fd);

/* ---- Net config ---- */
typedef enum { NET_DHCP = 0, NET_STATIC = 1 } net_mode_t;
typedef struct {
    net_mode_t mode;
    char ip[16], netmask[16], gw[16], dns[16];
    char hostname[32];
} net_cfg_t;

esp_err_t net_cfg_load(net_cfg_t *out);
esp_err_t net_cfg_save(const net_cfg_t *cfg);
void      net_cfg_apply(esp_netif_t *netif, const net_cfg_t *cfg);

/* ---- HTTP server ---- */
esp_err_t http_api_start(void);

/* ---- OTA confirm: schedule an auto-mark-valid after stable uptime ---- */
void      ota_arm_auto_confirm(void);
