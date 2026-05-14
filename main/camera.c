#include "sdkconfig.h"
#if CONFIG_PROBE_CAMERA
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "probe.h"

static const char *TAG = "camera";
static bool s_ok = false;

esp_err_t probe_camera_init(void)
{
    /* Power the camera daughterboard before probing SCCB. Active-low EN
     * via P-FET high-side switch (per Waveshare schematic). */
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << CAM_PIN_PWR_EN,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);
    gpio_set_level(CAM_PIN_PWR_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    camera_config_t cfg = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,
        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,
        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,
        .xclk_freq_hz   = 20000000,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,
        .pixel_format   = PIXFORMAT_JPEG,
        .frame_size     = FRAMESIZE_SVGA,    /* 800x600 — safe default */
        .jpeg_quality   = 12,
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };
    esp_err_t e = esp_camera_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: 0x%x", e);
        return e;
    }
    s_ok = true;
    /* Default to hmirror = 1: lens faces the panel so the raw sensor output
     * lands left-right reversed for the operator. Runtime override via
     * POST /camera/controls {"hmirror": 0/1, "vflip": 0/1}. */
    sensor_t *sens = esp_camera_sensor_get();
    if (sens) {
        sens->set_hmirror(sens, 1);
        sens->set_vflip(sens, 0);
    }
    ESP_LOGI(TAG, "OV2640 ready (hmirror=1)");
    return ESP_OK;
}

static esp_err_t snapshot_get(httpd_req_t *r)
{
    if (!s_ok) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized"); return ESP_OK; }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "fb_get failed"); return ESP_OK; }
    httpd_resp_set_type(r, "image/jpeg");
    httpd_resp_set_hdr(r, "Content-Disposition", "inline; filename=snap.jpg");
    httpd_resp_send(r, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

#define BOUNDARY "frame"
/* Cap stream output at 5 fps. The UART bring-up workflow only needs a slow
 * visual feed of the panel; running at full sensor rate starves the httpd
 * accept loop (LwIP single-threaded) and locks out concurrent UART and
 * status requests for the duration of the stream. */
#define STREAM_FRAME_INTERVAL_MS 200

/* MJPEG streaming runs in its own FreeRTOS task via the IDF async-handler
 * API (esp_http_server: httpd_req_async_handler_begin/_complete). The HTTP
 * worker thread returns immediately so other handlers (UART read/write,
 * snapshot, status, OTA) keep serving while the stream is open.
 *
 * Without this split, the worker thread was wedged in the send-loop until
 * the client disconnected, which stalled the entire single-threaded httpd
 * accept queue and made the slug unreachable mid-stream.
 */
static void stream_task(void *arg)
{
    httpd_req_t *async = (httpd_req_t *)arg;
    httpd_resp_set_type(async, "multipart/x-mixed-replace; boundary=" BOUNDARY);

    char part_hdr[96];
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(STREAM_FRAME_INTERVAL_MS);

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        int n = snprintf(part_hdr, sizeof part_hdr,
                         "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
        if (httpd_resp_send_chunk(async, part_hdr, n) != ESP_OK ||
            httpd_resp_send_chunk(async, (const char *)fb->buf, fb->len) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }
        esp_camera_fb_return(fb);
        vTaskDelayUntil(&last, period);
    }
    httpd_resp_send_chunk(async, NULL, 0);
    httpd_req_async_handler_complete(async);
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *r)
{
    if (!s_ok) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized"); return ESP_OK; }

    httpd_req_t *async = NULL;
    esp_err_t e = httpd_req_async_handler_begin(r, &async);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "async_handler_begin: 0x%x", e);
        return ESP_FAIL;
    }
    /* 6 KiB stack is enough for camera_fb_get + send_chunk + a small part
     * header. Priority 5 matches the rest of the brainslug worker tasks. */
    if (xTaskCreate(stream_task, "cam_stream", 6144, async, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(cam_stream) failed");
        httpd_req_async_handler_complete(async);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "stream task spawn failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t info_get(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "initialized", s_ok);
    sensor_t *s = s_ok ? esp_camera_sensor_get() : NULL;
    if (s) {
        cJSON_AddNumberToObject(j, "pid", s->id.PID);
        cJSON_AddNumberToObject(j, "framesize", s->status.framesize);
        cJSON_AddNumberToObject(j, "quality", s->status.quality);
        cJSON_AddBoolToObject (j, "exposure_auto", s->status.aec);
        cJSON_AddNumberToObject(j, "exposure_value", s->status.aec_value);   /* manual AEC, 0..1200 */
        cJSON_AddBoolToObject (j, "gain_auto", s->status.agc);
        cJSON_AddNumberToObject(j, "gain_value", s->status.agc_gain);        /* manual AGC, 0..30 */
        cJSON_AddNumberToObject(j, "brightness", s->status.brightness);
        cJSON_AddNumberToObject(j, "contrast", s->status.contrast);
    }
    char *txt = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, txt ? txt : "{}");
    free(txt); cJSON_Delete(j);
    return ESP_OK;
}

/* Read JSON body, drive sensor settings. Accepted keys (any subset):
 *   exposure_auto:bool    -> set_exposure_ctrl
 *   exposure_value:int    -> set_aec_value (0..1200; only takes effect if auto=false)
 *   gain_auto:bool        -> set_gain_ctrl
 *   gain_value:int        -> set_agc_gain (0..30)
 *   brightness:int        -> -2..2
 *   contrast:int          -> -2..2
 *   saturation:int        -> -2..2
 *   quality:int           -> 0..63 (lower = better)
 *   framesize:int         -> see framesize_t enum
 */
static char *read_body_local(httpd_req_t *r, size_t max)
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

static esp_err_t controls_post(httpd_req_t *r)
{
    if (!s_ok) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized"); return ESP_OK; }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no sensor"); return ESP_OK; }

    char *body = read_body_local(r, 512);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body required"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_OK; }

    cJSON *it;
    if ((it = cJSON_GetObjectItem(j, "exposure_auto")) && cJSON_IsBool(it))
        s->set_exposure_ctrl(s, cJSON_IsTrue(it));
    if ((it = cJSON_GetObjectItem(j, "exposure_value")) && cJSON_IsNumber(it))
        s->set_aec_value(s, it->valueint);
    if ((it = cJSON_GetObjectItem(j, "gain_auto")) && cJSON_IsBool(it))
        s->set_gain_ctrl(s, cJSON_IsTrue(it));
    if ((it = cJSON_GetObjectItem(j, "gain_value")) && cJSON_IsNumber(it))
        s->set_agc_gain(s, it->valueint);
    if ((it = cJSON_GetObjectItem(j, "brightness")) && cJSON_IsNumber(it))
        s->set_brightness(s, it->valueint);
    if ((it = cJSON_GetObjectItem(j, "contrast")) && cJSON_IsNumber(it))
        s->set_contrast(s, it->valueint);
    if ((it = cJSON_GetObjectItem(j, "saturation")) && cJSON_IsNumber(it))
        s->set_saturation(s, it->valueint);
    if ((it = cJSON_GetObjectItem(j, "quality")) && cJSON_IsNumber(it))
        s->set_quality(s, it->valueint);
    if ((it = cJSON_GetObjectItem(j, "framesize")) && cJSON_IsNumber(it))
        s->set_framesize(s, (framesize_t)it->valueint);
    if ((it = cJSON_GetObjectItem(j, "hmirror")) && (cJSON_IsBool(it) || cJSON_IsNumber(it)))
        s->set_hmirror(s, cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint != 0));
    if ((it = cJSON_GetObjectItem(j, "vflip")) && (cJSON_IsBool(it) || cJSON_IsNumber(it)))
        s->set_vflip(s, cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint != 0));
    cJSON_Delete(j);

    return info_get(r);
}

void probe_camera_register_routes(httpd_handle_t srv)
{
    httpd_uri_t routes[] = {
        { "/camera/info",     HTTP_GET,  info_get,      NULL, false, false, NULL },
        { "/camera/snapshot", HTTP_GET,  snapshot_get,  NULL, false, false, NULL },
        { "/camera/stream",   HTTP_GET,  stream_get,    NULL, false, false, NULL },
        { "/camera/controls", HTTP_POST, controls_post, NULL, false, false, NULL },
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; ++i)
        httpd_register_uri_handler(srv, &routes[i]);
}

#endif /* CONFIG_PROBE_CAMERA */
