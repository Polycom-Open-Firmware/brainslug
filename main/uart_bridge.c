#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "probe.h"

static const char *TAG = "uart-bridge";

static struct {
    int baud, data, parity, stop, tx, rx;
} s = {
    .baud = UART_DEFAULT_BAUD, .data = 8, .parity = 0, .stop = 1,
    .tx = UART_DEFAULT_TX_GPIO, .rx = UART_DEFAULT_RX_GPIO,
};

static esp_err_t apply(void)
{
    uart_config_t cfg = {
        .baud_rate  = s.baud,
        .data_bits  = (s.data == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS,
        .parity     = (s.parity == 1) ? UART_PARITY_ODD :
                      (s.parity == 2) ? UART_PARITY_EVEN : UART_PARITY_DISABLE,
        .stop_bits  = (s.stop == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_param_config(UART_BRIDGE_PORT, &cfg);
    if (e) return e;
    return uart_set_pin(UART_BRIDGE_PORT, s.tx, s.rx,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

esp_err_t uart_bridge_init(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_PORT,
                                        UART_RX_RING_BYTES, 0, 0, NULL, 0));
    esp_err_t e = apply();
    ESP_LOGI(TAG, "uart%d %d 8N1 tx=%d rx=%d", UART_BRIDGE_PORT, s.baud, s.tx, s.rx);
    return e;
}

esp_err_t uart_bridge_reconfigure(int baud, int data, int parity, int stop,
                                  int tx, int rx)
{
    if (baud   > 0)              s.baud   = baud;
    if (data   == 7 || data == 8) s.data  = data;
    if (parity >= 0 && parity <= 2) s.parity = parity;
    if (stop   == 1 || stop == 2) s.stop  = stop;
    if (tx     >= 0)             s.tx     = tx;
    if (rx     >= 0)             s.rx     = rx;
    return apply();
}

int uart_bridge_write(const uint8_t *buf, size_t len)
{
    return uart_write_bytes(UART_BRIDGE_PORT, (const char *)buf, len);
}

int uart_bridge_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    return uart_read_bytes(UART_BRIDGE_PORT, buf, max_len,
                           pdMS_TO_TICKS(timeout_ms));
}

void uart_bridge_get_config(int *baud, int *data, int *parity, int *stop,
                            int *tx, int *rx)
{
    if (baud)   *baud   = s.baud;
    if (data)   *data   = s.data;
    if (parity) *parity = s.parity;
    if (stop)   *stop   = s.stop;
    if (tx)     *tx     = s.tx;
    if (rx)     *rx     = s.rx;
}
