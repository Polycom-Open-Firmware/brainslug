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
    ESP_LOGI(TAG, "OV2640 ready");
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
static esp_err_t stream_get(httpd_req_t *r)
{
    if (!s_ok) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized"); return ESP_OK; }
    httpd_resp_set_type(r, "multipart/x-mixed-replace; boundary=" BOUNDARY);

    char part_hdr[96];
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        int n = snprintf(part_hdr, sizeof part_hdr,
                         "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
        if (httpd_resp_send_chunk(r, part_hdr, n) != ESP_OK ||
            httpd_resp_send_chunk(r, (const char *)fb->buf, fb->len) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }
        esp_camera_fb_return(fb);
    }
    httpd_resp_send_chunk(r, NULL, 0);
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
    }
    char *txt = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, txt ? txt : "{}");
    free(txt); cJSON_Delete(j);
    return ESP_OK;
}

void probe_camera_register_routes(httpd_handle_t srv)
{
    httpd_uri_t routes[] = {
        { "/camera/info",     HTTP_GET, info_get,     NULL, false, false, NULL },
        { "/camera/snapshot", HTTP_GET, snapshot_get, NULL, false, false, NULL },
        { "/camera/stream",   HTTP_GET, stream_get,   NULL, false, false, NULL },
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; ++i)
        httpd_register_uri_handler(srv, &routes[i]);
}

#endif /* CONFIG_PROBE_CAMERA */
