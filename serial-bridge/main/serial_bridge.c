/*
 * slug-1 (Silicognition wESP32 / ESP32) as a DUMB USB<->serial adapter
 * for Polycom Trio C60-#2 (the unmodified-stock live oracle).
 *
 *   UART1  GPIO33 (TX) / GPIO32 (RX)  <-->  C60-#2 ttymxc1 (115200 8N1)
 *   UART0  default TXD0/RXD0          -->   wESP32 onboard USB-serial
 *                                            -> host /dev/ttyUSB
 *
 * No Ethernet, no HTTP, no brainslug — a transparent byte pump. The
 * brainslug network path is unnecessary indirection when all we need is
 * C60-#2's console; this backup drops it entirely (see LAB.md).
 *
 * Console + bootloader logging is disabled in sdkconfig.defaults
 * (CONFIG_ESP_CONSOLE_UART_NONE / *_LOG_LEVEL_NONE) so the host sees a
 * clean C60-#2 stream with no ESP boot spam.
 *
 * GPIO16/17 on the wESP32 are the RMII MDC/MDIO (Ethernet) — NOT used
 * here; UART1 stays on the wESP32 UART defaults 33/32.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#define U_DEV    UART_NUM_1   /* to C60-#2 ttymxc1 */
#define U_HOST   UART_NUM_0   /* to host USB-serial */
#define DEV_TX   33
#define DEV_RX   32
#define BAUD     115200
#define BUF      1024

static void cfg(uart_port_t p, int tx, int rx)
{
    uart_config_t c = {
        .baud_rate  = BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(p, 8192, 8192, 0, NULL, 0);
    uart_param_config(p, &c);
    if (tx >= 0)
        uart_set_pin(p, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void app_main(void)
{
    cfg(U_HOST, -1, -1);            /* UART0 keeps default pins -> USB-serial */
    cfg(U_DEV, DEV_TX, DEV_RX);     /* UART1 -> C60-#2 ttymxc1 */

    static uint8_t b[BUF];
    for (;;) {
        int n = uart_read_bytes(U_DEV, b, sizeof b, pdMS_TO_TICKS(5));
        if (n > 0)
            uart_write_bytes(U_HOST, (const char *)b, n);
        n = uart_read_bytes(U_HOST, b, sizeof b, pdMS_TO_TICKS(5));
        if (n > 0)
            uart_write_bytes(U_DEV, (const char *)b, n);
    }
}
