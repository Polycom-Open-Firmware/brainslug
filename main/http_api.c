#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "probe.h"

static const char *TAG = "http-api";
static httpd_handle_t s_srv = NULL;

/* GPIO whitelist — board-specific reservations live in eth_init.c. */
static bool pin_allowed(int g) { return !probe_pin_reserved(g); }

static esp_err_t send_json(httpd_req_t *r, cJSON *j, int status)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    if (status == 400) httpd_resp_set_status(r, "400 Bad Request");
    else if (status == 404) httpd_resp_set_status(r, "404 Not Found");
    else if (status == 500) httpd_resp_set_status(r, "500 Internal Server Error");
    httpd_resp_sendstr(r, s ? s : "{}");
    free(s); cJSON_Delete(j);
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

/* Extract the {N} from /uart/N/... — returns -1 on malformed URI. */
static int uri_uart_port(const char *uri)
{
    const char *p = strstr(uri, "/uart/");
    if (!p) return -1;
    p += 6;
    if (*p < '1' || *p > '9') return -1;
    return *p - '0';
}

/* ===================== /info ===================== */
static esp_err_t info_get(httpd_req_t *r)
{
    const esp_app_desc_t *a = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "app", a->project_name);
    cJSON_AddStringToObject(j, "version", a->version);
    cJSON_AddStringToObject(j, "idf", a->idf_ver);
    cJSON_AddStringToObject(j, "build", a->date);
    cJSON_AddStringToObject(j, "running_partition", run ? run->label : "?");
    cJSON_AddStringToObject(j, "board", PROBE_BOARD_NAME);
    cJSON_AddNumberToObject(j, "uart_ports", UART_BRIDGE_PORTS);
#if CONFIG_PROBE_CAMERA
    cJSON_AddBoolToObject(j, "camera", true);
#else
    cJSON_AddBoolToObject(j, "camera", false);
#endif
    return send_json(r, j, 200);
}

/* ===================== /gpio ===================== */
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

    if (jmode && cJSON_IsString(jmode)) {
        gpio_config_t g = { .pin_bit_mask = 1ULL << pin,
                            .intr_type = GPIO_INTR_DISABLE,
                            .mode = GPIO_MODE_INPUT,
                            .pull_up_en = 0, .pull_down_en = 0 };
        const char *m = jmode->valuestring;
        if      (!strcmp(m, "out"))  g.mode = GPIO_MODE_OUTPUT;
        else if (!strcmp(m, "in"))   g.mode = GPIO_MODE_INPUT;
        else if (!strcmp(m, "in_pu")){g.mode = GPIO_MODE_INPUT; g.pull_up_en = 1; }
        else if (!strcmp(m, "in_pd")){g.mode = GPIO_MODE_INPUT; g.pull_down_en = 1; }
        else if (!strcmp(m, "od"))   g.mode = GPIO_MODE_OUTPUT_OD;
        else { cJSON_Delete(j); return send_err(r, 400, "bad mode"); }
        gpio_config(&g);
    }
    if (jlvl && cJSON_IsNumber(jlvl)) gpio_set_level(pin, jlvl->valueint ? 1 : 0);
    cJSON_Delete(j);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "pin", pin);
    cJSON_AddNumberToObject(resp, "level", gpio_get_level(pin));
    return send_json(r, resp, 200);
}

/* ===================== /uart/N/... ===================== */
static cJSON *uart_cfg_json(int port)
{
    uart_cfg_t c; uart_bridge_get_config(port, &c);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "port", port);
    cJSON_AddNumberToObject(j, "baud", c.baud);
    cJSON_AddNumberToObject(j, "data", c.data);
    cJSON_AddNumberToObject(j, "parity", c.parity);
    cJSON_AddNumberToObject(j, "stop", c.stop);
    cJSON_AddNumberToObject(j, "tx_gpio", c.tx);
    cJSON_AddNumberToObject(j, "rx_gpio", c.rx);
    return j;
}

static esp_err_t uart_cfg_get(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
    return send_json(r, uart_cfg_json(port), 200);
}

static int json_int_or(cJSON *j, const char *k, int dflt)
{
    cJSON *x = cJSON_GetObjectItem(j, k);
    return (x && cJSON_IsNumber(x)) ? x->valueint : dflt;
}

static esp_err_t uart_cfg_post(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
    char *body = read_body(r, 256);
    if (!body) return send_err(r, 400, "body required");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(r, 400, "bad json");

    uart_cfg_t in = {
        .baud   = json_int_or(j, "baud",    -1),
        .data   = json_int_or(j, "data",    -1),
        .parity = json_int_or(j, "parity",  -1),
        .stop   = json_int_or(j, "stop",    -1),
        .tx     = json_int_or(j, "tx_gpio", -1),
        .rx     = json_int_or(j, "rx_gpio", -1),
    };
    cJSON_Delete(j);

    if (in.tx >= 0 && !pin_allowed(in.tx)) return send_err(r, 400, "bad tx pin");
    if (in.rx >= 0 && !pin_allowed(in.rx)) return send_err(r, 400, "bad rx pin");

    if (uart_bridge_reconfigure(port, &in) != ESP_OK)
        return send_err(r, 500, "reconfigure failed");
    return send_json(r, uart_cfg_json(port), 200);
}

static esp_err_t uart_write_post(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
    if (r->content_len == 0 || r->content_len > 16384)
        return send_err(r, 400, "0 < len <= 16384");
    uint8_t buf[1024];
    size_t remaining = r->content_len, written = 0;
    while (remaining) {
        size_t want = remaining > sizeof buf ? sizeof buf : remaining;
        int n = httpd_req_recv(r, (char *)buf, want);
        if (n <= 0) return send_err(r, 500, "recv failed");
        int w = uart_bridge_write(port, buf, n);
        if (w < 0) return send_err(r, 500, "uart write failed");
        written += w; remaining -= n;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "port", port);
    cJSON_AddNumberToObject(j, "written", written);
    return send_json(r, j, 200);
}

static esp_err_t uart_swap_post(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
    uart_cfg_t cur; uart_bridge_get_config(port, &cur);
    uart_cfg_t in = cur;
    in.tx = cur.rx;
    in.rx = cur.tx;
    if (uart_bridge_reconfigure(port, &in) != ESP_OK)
        return send_err(r, 500, "reconfigure failed");
    return send_json(r, uart_cfg_json(port), 200);
}

/* POST /uart/<n>/break?ms=N — assert UART BREAK for N ms (default 250,
 * clamped to 5000). Used by sysrq-b flows. Empty body. */
static esp_err_t uart_break_post(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
    uint32_t ms = 250;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "ms", v, sizeof v) == ESP_OK) {
        int n = atoi(v);
        if (n > 0) ms = (uint32_t)n;
    }
    if (uart_bridge_break(port, ms) != ESP_OK)
        return send_err(r, 500, "break failed");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "port", port);
    cJSON_AddNumberToObject(j, "break_ms", (double)ms);
    return send_json(r, j, 200);
}

/* POST /uart/<n>/invert?rx=0|1&tx=0|1 — set TTL-level inversion. */
static esp_err_t uart_invert_post(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
    int rx = 0, tx = 0;
    char q[64], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "rx", v, sizeof v) == ESP_OK) rx = atoi(v);
        if (httpd_query_key_value(q, "tx", v, sizeof v) == ESP_OK) tx = atoi(v);
    }
    if (uart_bridge_set_invert(port, rx, tx) != ESP_OK)
        return send_err(r, 500, "invert failed");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "port", port);
    cJSON_AddBoolToObject(j, "rx_inv", rx != 0);
    cJSON_AddBoolToObject(j, "tx_inv", tx != 0);
    return send_json(r, j, 200);
}

static esp_err_t uart_read_get(httpd_req_t *r)
{
    int port = uri_uart_port(r->uri);
    if (!uart_port_valid(port)) return send_err(r, 404, "bad uart port");
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
    int n = uart_bridge_read(port, buf, max, tmo);
    httpd_resp_set_type(r, "application/octet-stream");
    char hdr[16]; snprintf(hdr, sizeof hdr, "%d", n < 0 ? 0 : n);
    httpd_resp_set_hdr(r, "X-UART-Bytes", hdr);
    httpd_resp_send(r, (const char *)buf, n < 0 ? 0 : n);
    free(buf);
    return ESP_OK;
}

/* Full-duplex WS at /uart/N/ws.
 *   client -> slug: binary/text frames flushed to UART TX via uart_bridge_write.
 *   slug -> client: RX bytes pushed as binary frames by the per-port reader
 *                   task in uart_bridge.c (it owns the sink fd).
 * Single sink per port, last-wins: a new connection evicts the old one.
 *
 * Port number is carried in user_ctx (set at registration). r->uri is NOT
 * usable here — ESP-IDF's init_req zeroes it on every incoming WS frame, so
 * parsing the URI string would yield -1 and we'd 404-close the WS session. */
static esp_err_t uart_ws_handler(httpd_req_t *r)
{
    int port = (int)(intptr_t)r->user_ctx;
    if (!uart_port_valid(port)) return ESP_FAIL;

    int fd = httpd_req_to_sockfd(r);

    if (r->method == HTTP_GET) {
        /* Initial upgrade — claim the sink. */
        uart_bridge_set_ws_sink(port, s_srv, fd);
        ESP_LOGI(TAG, "ws open uart%d fd=%d", port, fd);
        return ESP_OK;
    }

    /* Frame arrived. Peek length first, then alloc + recv. */
    httpd_ws_frame_t f = {0};
    esp_err_t e = httpd_ws_recv_frame(r, &f, 0);
    if (e != ESP_OK) return e;

    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        uart_bridge_set_ws_sink(port, NULL, -1);
        return ESP_OK;
    }
    if (f.type == HTTPD_WS_TYPE_PING || f.type == HTTPD_WS_TYPE_PONG)
        return ESP_OK;  /* httpd auto-handles control frames; nothing to do. */

    if (f.len == 0) return ESP_OK;
    f.payload = malloc(f.len);
    if (!f.payload) return ESP_ERR_NO_MEM;
    e = httpd_ws_recv_frame(r, &f, f.len);
    if (e == ESP_OK && (f.type == HTTPD_WS_TYPE_BINARY || f.type == HTTPD_WS_TYPE_TEXT)) {
        int w = uart_bridge_write(port, f.payload, f.len);
        if (w < 0)
            ESP_LOGW(TAG, "uart%d ws->tx write failed", port);
    }
    free(f.payload);
    return e;
}

/* ===================== /net ===================== */
static esp_err_t net_get(httpd_req_t *r)
{
    net_cfg_t c; net_cfg_load(&c);
    esp_netif_ip_info_t info = {0};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (nif) esp_netif_get_ip_info(nif, &info);
    char ip[16], nm[16], gw[16];
    esp_ip4addr_ntoa(&info.ip, ip, sizeof ip);
    esp_ip4addr_ntoa(&info.netmask, nm, sizeof nm);
    esp_ip4addr_ntoa(&info.gw, gw, sizeof gw);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", c.mode == NET_STATIC ? "static" : "dhcp");
    cJSON_AddStringToObject(j, "hostname", c.hostname);
    cJSON_AddStringToObject(j, "configured_ip", c.ip);
    cJSON_AddStringToObject(j, "configured_netmask", c.netmask);
    cJSON_AddStringToObject(j, "configured_gw", c.gw);
    cJSON_AddStringToObject(j, "configured_dns", c.dns);
    cJSON_AddStringToObject(j, "current_ip", ip);
    cJSON_AddStringToObject(j, "current_netmask", nm);
    cJSON_AddStringToObject(j, "current_gw", gw);
    return send_json(r, j, 200);
}

static void copy_str(char *dst, size_t cap, cJSON *j, const char *k)
{
    cJSON *x = cJSON_GetObjectItem(j, k);
    if (x && cJSON_IsString(x) && x->valuestring)
        strncpy(dst, x->valuestring, cap - 1);
}

static esp_err_t net_post(httpd_req_t *r)
{
    char *body = read_body(r, 512);
    if (!body) return send_err(r, 400, "body required");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(r, 400, "bad json");

    net_cfg_t c; net_cfg_load(&c);
    cJSON *m = cJSON_GetObjectItem(j, "mode");
    if (m && cJSON_IsString(m)) {
        if      (!strcmp(m->valuestring, "static")) c.mode = NET_STATIC;
        else if (!strcmp(m->valuestring, "dhcp"))   c.mode = NET_DHCP;
        else { cJSON_Delete(j); return send_err(r, 400, "mode must be dhcp|static"); }
    }
    memset(c.ip, 0, sizeof c.ip);
    memset(c.netmask, 0, sizeof c.netmask);
    memset(c.gw, 0, sizeof c.gw);
    memset(c.dns, 0, sizeof c.dns);
    copy_str(c.ip,       sizeof c.ip,       j, "ip");
    copy_str(c.netmask,  sizeof c.netmask,  j, "netmask");
    copy_str(c.gw,       sizeof c.gw,       j, "gw");
    copy_str(c.dns,      sizeof c.dns,      j, "dns");
    copy_str(c.hostname, sizeof c.hostname, j, "hostname");
    if (!c.hostname[0]) strcpy(c.hostname, "wesp-probe");
    cJSON_Delete(j);

    if (c.mode == NET_STATIC && (!c.ip[0] || !c.netmask[0]))
        return send_err(r, 400, "static mode requires ip and netmask");

    if (net_cfg_save(&c) != ESP_OK) return send_err(r, 500, "nvs save failed");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "saved", true);
    cJSON_AddStringToObject(resp, "note", "reboot to apply");
    return send_json(r, resp, 200);
}

/* ===================== /ota ===================== */
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
        if ((e = esp_ota_write(h, (const void *)buf, n)) != ESP_OK) {
            esp_ota_abort(h);
            return send_err(r, 500, "ota_write failed");
        }
        total += n;
    }
    if (n < 0) { esp_ota_abort(h); return send_err(r, 500, "recv failed"); }
    if ((e = esp_ota_end(h)) != ESP_OK)
        return send_err(r, 500, "ota_end failed (image invalid?)");
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

/* ===================== /reboot ===================== */
static esp_err_t reboot_post(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ===================== / (embedded index.html) ===================== */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t index_get(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, (const char *)index_html_start,
                              index_html_end - index_html_start);
}

esp_err_t http_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.max_uri_handlers  = 32;
    /* Headroom for: 1 MJPEG stream, up to 2 UART WS clients, OTA upload, and
     * a few concurrent short HTTP polls without LRU-evicting each other.
     * lwip MAX_SOCK is 16; reserve a few for system use. */
    cfg.max_open_sockets  = 13;
    /* recv_wait shrunk from default so a half-open HTTP request is reaped
     * before the socket cap fills. send_wait stays generous: it bounds how
     * long a single outbound WS frame can block on a slow client; setting
     * it too low triggers the sink-detach path on benign micro-stalls. */
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 10;
    cfg.stack_size        = 8192;
    cfg.lru_purge_enable  = true;
    ESP_ERROR_CHECK(httpd_start(&s_srv, &cfg));

    httpd_uri_t routes[] = {
        { "/",                HTTP_GET,  index_get,       NULL, false, false, NULL },
        { "/info",            HTTP_GET,  info_get,        NULL, false, false, NULL },
        { "/gpio",            HTTP_GET,  gpio_get,        NULL, false, false, NULL },
        { "/gpio",            HTTP_POST, gpio_post,       NULL, false, false, NULL },
        { "/net",             HTTP_GET,  net_get,         NULL, false, false, NULL },
        { "/net",             HTTP_POST, net_post,        NULL, false, false, NULL },
        { "/ota",             HTTP_POST, ota_post,        NULL, false, false, NULL },
        { "/reboot",          HTTP_POST, reboot_post,     NULL, false, false, NULL },
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_srv, &routes[i]));

    /* Per-port UART routes — registered explicitly because ESP-IDF's
     * httpd wildcard match doesn't treat '?' as a single-char wildcard. */
    for (int port = 1; port <= UART_BRIDGE_PORTS; ++port) {
        char *cfg_uri  = malloc(24); snprintf(cfg_uri,  24, "/uart/%d/config", port);
        char *wr_uri   = malloc(24); snprintf(wr_uri,   24, "/uart/%d/write",  port);
        char *rd_uri   = malloc(24); snprintf(rd_uri,   24, "/uart/%d/read",   port);
        char *ws_uri   = malloc(24); snprintf(ws_uri,   24, "/uart/%d/ws",     port);
        char *sw_uri   = malloc(24); snprintf(sw_uri,   24, "/uart/%d/swap",   port);
        char *br_uri   = malloc(24); snprintf(br_uri,   24, "/uart/%d/break",  port);
        char *iv_uri   = malloc(24); snprintf(iv_uri,   24, "/uart/%d/invert", port);
        httpd_uri_t uart_routes[] = {
            { cfg_uri, HTTP_GET,  uart_cfg_get,    NULL, false, false, NULL },
            { cfg_uri, HTTP_POST, uart_cfg_post,   NULL, false, false, NULL },
            { wr_uri,  HTTP_POST, uart_write_post, NULL, false, false, NULL },
            { rd_uri,  HTTP_GET,  uart_read_get,   NULL, false, false, NULL },
            { ws_uri,  HTTP_GET,  uart_ws_handler, (void *)(intptr_t)port, true, false, NULL },
            { sw_uri,  HTTP_POST, uart_swap_post,  NULL, false, false, NULL },
            { br_uri,  HTTP_POST, uart_break_post, NULL, false, false, NULL },
            { iv_uri,  HTTP_POST, uart_invert_post,NULL, false, false, NULL },
        };
        for (size_t i = 0; i < sizeof uart_routes / sizeof *uart_routes; ++i)
            ESP_ERROR_CHECK(httpd_register_uri_handler(s_srv, &uart_routes[i]));
    }

#if CONFIG_PROBE_CAMERA
    probe_camera_register_routes(s_srv);
#endif
#if CONFIG_PROBE_AUDIO
    audio_register_routes(s_srv);
#endif

    ESP_LOGI(TAG, "http api on :80");
    return ESP_OK;
}
