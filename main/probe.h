#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* WESP32 (Silicognition wESP32) Ethernet pinout.
 * LAN8720, external 50 MHz oscillator drives RMII REF_CLK into GPIO0.
 * No PHY power-enable GPIO on this board.
 */
#define WESP_ETH_PHY_ADDR     0
#define WESP_ETH_MDC_GPIO     16
#define WESP_ETH_MDIO_GPIO    17
#define WESP_ETH_PHY_RST_GPIO -1

/* UART bridge defaults — pins are user-configurable at runtime via API,
 * but we need sane boot defaults. Avoid GPIOs used by Ethernet RMII
 * (0,16,17,18,19,21,22,23,25,26,27) and strapping pins where reasonable.
 */
#define UART_BRIDGE_PORT      UART_NUM_1
#define UART_DEFAULT_TX_GPIO  33
#define UART_DEFAULT_RX_GPIO  32
#define UART_DEFAULT_BAUD     115200
#define UART_RX_RING_BYTES    8192

/* Pins exposed via /gpio API — anything not claimed by Ethernet/UART/flash. */

esp_err_t uart_bridge_init(void);
esp_err_t uart_bridge_reconfigure(int baud, int data_bits, int parity, int stop_bits,
                                  int tx_gpio, int rx_gpio);
int       uart_bridge_write(const uint8_t *buf, size_t len);
int       uart_bridge_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms);
void      uart_bridge_get_config(int *baud, int *data, int *parity, int *stop,
                                 int *tx_gpio, int *rx_gpio);

esp_err_t http_api_start(void);
