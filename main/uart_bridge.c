#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "probe.h"

static const char *TAG = "uart-bridge";

typedef struct {
    uart_port_t hw;
    uart_cfg_t  cfg;
    int            applied_tx, applied_rx; /* last pins we routed, -1 = none */
    httpd_handle_t ws_srv;
    int            ws_fd;     /* -1 when no streamer */
    TaskHandle_t   reader;
    const char    *nvs_key;
} port_t;

static port_t g_ports[UART_BRIDGE_PORTS] = {
    { .hw = UART_NUM_1, .cfg = { UART_DEFAULT_BAUD, 8, 0, 1, UART1_DEFAULT_TX_GPIO, UART1_DEFAULT_RX_GPIO },
      .applied_tx = -1, .applied_rx = -1, .ws_fd = -1, .nvs_key = "uart1" },
    { .hw = UART_NUM_2, .cfg = { UART_DEFAULT_BAUD, 8, 0, 1, UART2_DEFAULT_TX_GPIO, UART2_DEFAULT_RX_GPIO },
      .applied_tx = -1, .applied_rx = -1, .ws_fd = -1, .nvs_key = "uart2" },
};

bool uart_port_valid(int port) { return port >= 1 && port <= UART_BRIDGE_PORTS; }
static port_t *P(int port) { return uart_port_valid(port) ? &g_ports[port - 1] : NULL; }

static esp_err_t apply(port_t *p)
{
    uart_config_t cfg = {
        .baud_rate  = p->cfg.baud,
        .data_bits  = (p->cfg.data == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS,
        .parity     = (p->cfg.parity == 1) ? UART_PARITY_ODD :
                      (p->cfg.parity == 2) ? UART_PARITY_EVEN : UART_PARITY_DISABLE,
        .stop_bits  = (p->cfg.stop == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_param_config(p->hw, &cfg);
    if (e) return e;
    /* Disconnect any previous pin we owned before pointing the peripheral
     * elsewhere, so an old GPIO doesn't keep this UART's signal latched.
     * Crucial when /uart/N/config moves a port off pins that another UART
     * also defaulted to — otherwise the leftover gpio_matrix_out for that
     * pin sticks around and hijacks whichever UART claims it next. */
    if (p->applied_tx >= 0 && p->applied_tx != p->cfg.tx) gpio_reset_pin(p->applied_tx);
    if (p->applied_rx >= 0 && p->applied_rx != p->cfg.rx) gpio_reset_pin(p->applied_rx);
    e = uart_set_pin(p->hw, p->cfg.tx, p->cfg.rx,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e == ESP_OK) { p->applied_tx = p->cfg.tx; p->applied_rx = p->cfg.rx; }
    return e;
}

static void cfg_load_nvs(port_t *p)
{
    nvs_handle_t h;
    if (nvs_open("probe", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof p->cfg;
    if (nvs_get_blob(h, p->nvs_key, &p->cfg, &sz) != ESP_OK || sz != sizeof p->cfg) {
        ESP_LOGI(TAG, "no saved cfg for %s, using defaults", p->nvs_key);
    }
    nvs_close(h);
}

static void cfg_save_nvs(const port_t *p)
{
    nvs_handle_t h;
    if (nvs_open("probe", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, p->nvs_key, &p->cfg, sizeof p->cfg);
    nvs_commit(h);
    nvs_close(h);
}

/* Push received bytes to the websocket sink, if any. Runs in reader task. */
typedef struct { httpd_handle_t srv; int fd; int port; uint8_t *data; size_t len; } ws_send_t;
static void ws_send_work(void *arg)
{
    ws_send_t *s = arg;
    httpd_ws_frame_t f = {
        .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_BINARY,
        .payload = s->data, .len = s->len,
    };
    esp_err_t e = httpd_ws_send_frame_async(s->srv, s->fd, &f);
    if (e != ESP_OK) {
        /* Send failed — client is dead. Detach the sink (only if it's still us)
         * so a reconnect doesn't get rejected and the reader_task stops queuing
         * doomed work items. Best-effort close to recycle the httpd socket. */
        port_t *p = P(s->port);
        if (p && p->ws_fd == s->fd) { p->ws_srv = NULL; p->ws_fd = -1; }
        if (s->srv) httpd_sess_trigger_close(s->srv, s->fd);
        ESP_LOGW(TAG, "ws fd=%d send failed (%s); sink detached", s->fd, esp_err_to_name(e));
    }
    free(s->data); free(s);
}

static void reader_task(void *arg)
{
    port_t *p = arg;
    uint8_t buf[512];
    while (1) {
        /* Don't drain the RX ring unless a WS sink is consuming it —
         * otherwise polled /uart/N/read sees nothing. */
        if (p->ws_fd < 0 || !p->ws_srv) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int n = uart_read_bytes(p->hw, buf, sizeof buf, pdMS_TO_TICKS(20));
        if (n <= 0) continue;
        ws_send_t *s = malloc(sizeof *s);
        if (!s) continue;
        s->srv = p->ws_srv; s->fd = p->ws_fd;
        s->port = (int)(p - g_ports) + 1;
        s->data = malloc(n);
        if (!s->data) { free(s); continue; }
        memcpy(s->data, buf, n); s->len = n;
        if (httpd_queue_work(s->srv, ws_send_work, s) != ESP_OK) {
            free(s->data); free(s);
        }
    }
}

esp_err_t uart_bridge_init_all(void)
{
    for (int i = 0; i < UART_BRIDGE_PORTS; ++i) {
        port_t *p = &g_ports[i];
        cfg_load_nvs(p);
        ESP_ERROR_CHECK(uart_driver_install(p->hw, UART_RX_RING_BYTES, 0, 0, NULL, 0));
        esp_err_t e = apply(p);
        if (e) return e;
        ESP_LOGI(TAG, "uart%d %d %d%c%d tx=%d rx=%d",
                 (int)p->hw, p->cfg.baud, p->cfg.data,
                 p->cfg.parity == 0 ? 'N' : p->cfg.parity == 1 ? 'O' : 'E',
                 p->cfg.stop, p->cfg.tx, p->cfg.rx);
        char name[16]; snprintf(name, sizeof name, "uart%d-rd", (int)p->hw);
        xTaskCreate(reader_task, name, 4096, p, 5, &p->reader);
    }
    return ESP_OK;
}

esp_err_t uart_bridge_reconfigure(int port, const uart_cfg_t *in)
{
    port_t *p = P(port); if (!p) return ESP_ERR_INVALID_ARG;
    uart_cfg_t n = p->cfg;
    if (in->baud   > 0)              n.baud   = in->baud;
    if (in->data   == 7 || in->data == 8) n.data = in->data;
    if (in->parity >= 0 && in->parity <= 2) n.parity = in->parity;
    if (in->stop   == 1 || in->stop == 2) n.stop  = in->stop;
    if (in->tx     >= 0)             n.tx     = in->tx;
    if (in->rx     >= 0)             n.rx     = in->rx;
    p->cfg = n;
    esp_err_t e = apply(p);
    if (e == ESP_OK) cfg_save_nvs(p);
    return e;
}

int uart_bridge_write(int port, const uint8_t *buf, size_t len)
{
    port_t *p = P(port); if (!p) return -1;
    return uart_write_bytes(p->hw, (const char *)buf, len);
}

int uart_bridge_read(int port, uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    port_t *p = P(port); if (!p) return -1;
    /* The reader task drains the RX ring into the WS sink while it's active.
     * If no WS sink, polling /uart/N/read works. If a sink is active, the
     * polling endpoint will see nothing — that's intentional. */
    if (p->ws_fd >= 0) return 0;
    return uart_read_bytes(p->hw, buf, max_len, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t uart_bridge_get_config(int port, uart_cfg_t *out)
{
    port_t *p = P(port); if (!p) return ESP_ERR_INVALID_ARG;
    *out = p->cfg;
    return ESP_OK;
}

void uart_bridge_set_ws_sink(int port, httpd_handle_t srv, int fd)
{
    port_t *p = P(port); if (!p) return;
    /* Last-wins: a new sink kicks the old one. Trigger close on the previous
     * fd so the kernel reaps the socket and httpd can hand it out again. */
    if (p->ws_fd >= 0 && fd >= 0 && fd != p->ws_fd && p->ws_srv) {
        httpd_sess_trigger_close(p->ws_srv, p->ws_fd);
        ESP_LOGI(TAG, "uart%d ws sink evicted fd=%d", (int)p->hw, p->ws_fd);
    }
    p->ws_srv = srv;
    p->ws_fd  = fd;
    if (fd >= 0) ESP_LOGI(TAG, "uart%d ws sink fd=%d", (int)p->hw, fd);
}

/* Toggle TTL-level inversion on RX and/or TX. Use when the wire under test
 * carries inverted polarity (e.g. dirt-cheap RS-232 -> TTL adapter that
 * forgets the inverter, or a downstream level shifter that flips idle). */
esp_err_t uart_bridge_set_invert(int port, int rx_inv, int tx_inv)
{
    port_t *p = P(port); if (!p) return ESP_ERR_INVALID_ARG;
    uint32_t mask = 0;
    if (rx_inv) mask |= UART_SIGNAL_RXD_INV;
    if (tx_inv) mask |= UART_SIGNAL_TXD_INV;
    return uart_set_line_inverse(p->hw, mask);
}

/* Drive TXD low for `ms` to assert a UART BREAK. Needed by sysrq-b flows
 * that nudge a hung Linux into a forced reset. Implemented via
 * uart_set_line_inverse: inverting TXD while idle makes it sit LOW, which
 * is the BREAK condition; uninverting restores normal idle-HIGH. */
esp_err_t uart_bridge_break(int port, uint32_t ms)
{
    port_t *p = P(port); if (!p) return ESP_ERR_INVALID_ARG;
    if (ms == 0)    ms = 250;
    if (ms > 5000)  ms = 5000;
    uart_wait_tx_done(p->hw, pdMS_TO_TICKS(100));
    esp_err_t e = uart_set_line_inverse(p->hw, UART_SIGNAL_TXD_INV);
    if (e) return e;
    vTaskDelay(pdMS_TO_TICKS(ms));
    return uart_set_line_inverse(p->hw, 0);
}
