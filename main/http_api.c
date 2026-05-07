#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "probe.h"

static const char *TAG = "http-api";

/* Pins safe for general GPIO use on WESP32:
 *   - Avoid Ethernet RMII: 0, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27
 *   - Avoid flash: 6-11
 *   - UART bridge defaults occupy 32, 33 (changeable)
 *   - Strapping pins (0, 2, 5, 12, 15) work but mind boot levels
 * Allow any user pin and let them shoot their foot — this is a debug probe.
 */
static bool pin_allowed(int g)
{
    if (g < 0 || g > 39) return false;
    if (g >= 6 && g <= 11) return false;          /* SPI flash */
    if (g == 16 || g == 17) return false;         /* MDC/MDIO */
    if (g == 0 || g == 18 || g == 19 || g == 21 ||
        g == 22 || g == 23 || g == 25 || g == 26 || g == 27) return false; /* RMII */
    return true;
}

static esp_err_t send_json(httpd_req_t *r, cJSON *j, int status)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    if (status == 400) httpd_resp_set_status(r, "400 Bad Request");
    else if (status == 500) httpd_resp_set_status(r, "500 Internal Server Error");
    httpd_resp_sendstr(r, s ? s : "{}");
    free(s);
    cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t send_err(httpd_req_t *r, int code, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(r, j, code);
}

static char *read_body(httpd_req_t *r, size_t max)
{
    if (r->content_len == 0 || r->content_len > max) return NULL;
    char *buf = malloc(r->content_len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < (int)r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[got] = 0;
    return buf;
}

/* ---- /info ---- */
static esp_err_t info_get(httpd_req_t *r)
{
    const esp_app_desc_t *a = esp_app_get_description();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "app", a->project_name);
    cJSON_AddStringToObject(j, "version", a->version);
    cJSON_AddStringToObject(j, "idf", a->idf_ver);
    cJSON_AddStringToObject(j, "build", a->date);
    return send_json(r, j, 200);
}

/* ---- /gpio ----
 * GET  /gpio?pin=N           -> {"pin":N,"level":0|1}
 * POST /gpio  {"pin":N,"mode":"in|out|in_pu|in_pd","level":0|1}
 */
static esp_err_t gpio_get(httpd_req_t *r)
{
    char q[32]; int pin = -1;
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "pin", v, sizeof v) == ESP_OK) pin = atoi(v);
    }
    if (!pin_allowed(pin)) return send_err(r, 400, "bad or reserved pin");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "pin", pin);
    cJSON_AddNumberToObject(j, "level", gpio_get_level(pin));
    return send_json(r, j, 200);
}

static esp_err_t gpio_post(httpd_req_t *r)
{
    char *body = read_body(r, 256);
    if (!body) return send_err(r, 400, "body required");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(r, 400, "bad json");

    int pin = cJSON_GetObjectItem(j, "pin") ? cJSON_GetObjectItem(j, "pin")->valueint : -1;
    cJSON *jmode = cJSON_GetObjectItem(j, "mode");
    cJSON *jlvl  = cJSON_GetObjectItem(j, "level");

    if (!pin_allowed(pin)) { cJSON_Delete(j); return send_err(r, 400, "bad pin"); }

    gpio_config_t g = { .pin_bit_mask = 1ULL << pin,
                        .intr_type = GPIO_INTR_DISABLE,
                        .mode = GPIO_MODE_INPUT,
                        .pull_up_en = 0, .pull_down_en = 0 };
    if (jmode && cJSON_IsString(jmode)) {
        const char *m = jmode->valuestring;
        if (!strcmp(m, "out"))         g.mode = GPIO_MODE_OUTPUT;
        else if (!strcmp(m, "in"))     g.mode = GPIO_MODE_INPUT;
        else if (!strcmp(m, "in_pu")){ g.mode = GPIO_MODE_INPUT; g.pull_up_en = 1; }
        else if (!strcmp(m, "in_pd")){ g.mode = GPIO_MODE_INPUT; g.pull_down_en = 1; }
        else if (!strcmp(m, "od"))     g.mode = GPIO_MODE_OUTPUT_OD;
        else { cJSON_Delete(j); return send_err(r, 400, "bad mode"); }
        gpio_config(&g);
    }
    if (jlvl && cJSON_IsNumber(jlvl)) {
        gpio_set_level(pin, jlvl->valueint ? 1 : 0);
    }
    cJSON_Delete(j);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "pin", pin);
    cJSON_AddNumberToObject(resp, "level", gpio_get_level(pin));
    return send_json(r, resp, 200);
}

/* ---- /uart/config ---- */
static esp_err_t uart_cfg_get(httpd_req_t *r)
{
    int b, d, p, s, tx, rx;
    uart_bridge_get_config(&b, &d, &p, &s, &tx, &rx);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "baud", b);
    cJSON_AddNumberToObject(j, "data", d);
    cJSON_AddNumberToObject(j, "parity", p);
    cJSON_AddNumberToObject(j, "stop", s);
    cJSON_AddNumberToObject(j, "tx_gpio", tx);
    cJSON_AddNumberToObject(j, "rx_gpio", rx);
    return send_json(r, j, 200);
}

static int json_int_or(cJSON *j, const char *k, int dflt)
{
    cJSON *x = cJSON_GetObjectItem(j, k);
    return (x && cJSON_IsNumber(x)) ? x->valueint : dflt;
}

static esp_err_t uart_cfg_post(httpd_req_t *r)
{
    char *body = read_body(r, 256);
    if (!body) return send_err(r, 400, "body required");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(r, 400, "bad json");

    int baud = json_int_or(j, "baud", -1);
    int data = json_int_or(j, "data", -1);
    int par  = json_int_or(j, "parity", -1);
    int stop = json_int_or(j, "stop", -1);
    int tx   = json_int_or(j, "tx_gpio", -1);
    int rx   = json_int_or(j, "rx_gpio", -1);
    cJSON_Delete(j);

    if (tx >= 0 && !pin_allowed(tx)) return send_err(r, 400, "bad tx pin");
    if (rx >= 0 && !pin_allowed(rx)) return send_err(r, 400, "bad rx pin");

    esp_err_t e = uart_bridge_reconfigure(baud, data, par, stop, tx, rx);
    if (e != ESP_OK) return send_err(r, 500, "reconfigure failed");
    return uart_cfg_get(r);
}

/* ---- /uart/write — raw body bytes go out the UART ---- */
static esp_err_t uart_write_post(httpd_req_t *r)
{
    if (r->content_len == 0 || r->content_len > 16384)
        return send_err(r, 400, "0 < len <= 16384");
    uint8_t buf[1024];
    size_t remaining = r->content_len, written = 0;
    while (remaining) {
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        int n = httpd_req_recv(r, (char *)buf, want);
        if (n <= 0) return send_err(r, 500, "recv failed");
        int w = uart_bridge_write(buf, n);
        if (w < 0) return send_err(r, 500, "uart write failed");
        written += w; remaining -= n;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "written", written);
    return send_json(r, j, 200);
}

/* ---- /uart/read?max=N&timeout_ms=M — body is raw bytes ---- */
static esp_err_t uart_read_get(httpd_req_t *r)
{
    int max = 1024, tmo = 100;
    char q[64], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "max", v, sizeof v) == ESP_OK)        max = atoi(v);
        if (httpd_query_key_value(q, "timeout_ms", v, sizeof v) == ESP_OK) tmo = atoi(v);
    }
    if (max < 1 || max > 16384) max = 1024;
    if (tmo < 0 || tmo > 30000) tmo = 100;

    uint8_t *buf = malloc(max);
    if (!buf) return send_err(r, 500, "oom");
    int n = uart_bridge_read(buf, max, tmo);
    httpd_resp_set_type(r, "application/octet-stream");
    char hdr[16]; snprintf(hdr, sizeof hdr, "%d", n < 0 ? 0 : n);
    httpd_resp_set_hdr(r, "X-UART-Bytes", hdr);
    httpd_resp_send(r, (const char *)buf, n < 0 ? 0 : n);
    free(buf);
    return ESP_OK;
}

/* ---- /ota — POST raw firmware binary to flash next slot, then reboot ---- */
static esp_err_t ota_post(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return send_err(r, 500, "no ota partition");

    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) return send_err(r, 500, "ota_begin failed");

    char buf[2048];
    int total = 0, n;
    while ((n = httpd_req_recv(r, buf, sizeof buf)) > 0) {
        if ((e = esp_ota_write(h, buf, n)) != ESP_OK) {
            esp_ota_abort(h);
            return send_err(r, 500, "ota_write failed");
        }
        total += n;
    }
    if (n < 0) { esp_ota_abort(h); return send_err(r, 500, "recv failed"); }

    if ((e = esp_ota_end(h)) != ESP_OK) return send_err(r, 500, "ota_end failed (image invalid?)");
    if ((e = esp_ota_set_boot_partition(part)) != ESP_OK)
        return send_err(r, 500, "set_boot failed");

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "written", total);
    cJSON_AddStringToObject(j, "next", part->label);
    cJSON_AddBoolToObject(j, "rebooting", true);
    send_json(r, j, 200);

    ESP_LOGW(TAG, "OTA complete (%d bytes), rebooting", total);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---- /reboot ---- */
static esp_err_t reboot_post(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

esp_err_t http_api_start(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    cfg.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));

    httpd_uri_t routes[] = {
        { "/info",         HTTP_GET,  info_get,       NULL },
        { "/gpio",         HTTP_GET,  gpio_get,       NULL },
        { "/gpio",         HTTP_POST, gpio_post,      NULL },
        { "/uart/config",  HTTP_GET,  uart_cfg_get,   NULL },
        { "/uart/config",  HTTP_POST, uart_cfg_post,  NULL },
        { "/uart/write",   HTTP_POST, uart_write_post,NULL },
        { "/uart/read",    HTTP_GET,  uart_read_get,  NULL },
        { "/ota",          HTTP_POST, ota_post,       NULL },
        { "/reboot",       HTTP_POST, reboot_post,    NULL },
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &routes[i]));

    ESP_LOGI(TAG, "http api started on :80");
    return ESP_OK;
}
