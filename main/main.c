#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "probe.h"

static const char *TAG = "probe";
static EventGroupHandle_t s_net_evt;
#define NET_GOT_IP BIT0
static net_cfg_t s_netcfg;

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_net_evt, NET_GOT_IP);
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns init failed"); return; }
    const char *host = s_netcfg.hostname[0] ? s_netcfg.hostname : PROBE_HOSTNAME_DEFAULT;
    mdns_hostname_set(host);
    mdns_instance_name_set(PROBE_MDNS_INSTANCE);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_instance_name_set("_http", "_tcp", PROBE_MDNS_INSTANCE);
    ESP_LOGI(TAG, "mdns: %s.local", host);
}

static void ota_confirm_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(30000));
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "OTA image marked valid");
        else
            ESP_LOGW(TAG, "OTA mark-valid failed");
    }
    vTaskDelete(NULL);
}
void ota_arm_auto_confirm(void) { xTaskCreate(ota_confirm_task, "ota-cf", 3072, NULL, 3, NULL); }

void app_main(void)
{
    ESP_LOGI(TAG, "boot: board=%s", PROBE_BOARD_NAME);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_net_evt = xEventGroupCreate();

    net_cfg_load(&s_netcfg);
    if (!s_netcfg.hostname[0])
        strcpy(s_netcfg.hostname, PROBE_HOSTNAME_DEFAULT);

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
    probe_eth_start(&s_netcfg);

    ESP_ERROR_CHECK(uart_bridge_init_all());
#if CONFIG_PROBE_CAMERA
    if (probe_camera_init() != ESP_OK)
        ESP_LOGW(TAG, "camera init failed — /camera endpoints will return errors");
#endif

    xEventGroupWaitBits(s_net_evt, NET_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    mdns_start();
    ESP_ERROR_CHECK(http_api_start());
    ota_arm_auto_confirm();
    ESP_LOGI(TAG, "ready");
}
