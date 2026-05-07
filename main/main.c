#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "probe.h"

static const char *TAG = "wesp-probe";
static EventGroupHandle_t s_net_evt;
#define NET_GOT_IP BIT0
static esp_netif_t *s_eth_netif;
static net_cfg_t   s_netcfg;

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "eth link up");   break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGW(TAG, "eth link down"); break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "eth start");     break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "eth stop");      break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_net_evt, NET_GOT_IP);
}

static void ethernet_init(void)
{
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&ncfg);

    /* Apply hostname/static-IP before bringing the link up. */
    net_cfg_load(&s_netcfg);
    net_cfg_apply(s_eth_netif, &s_netcfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = WESP_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = WESP_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_mdc_gpio_num  = WESP_ETH_MDC_GPIO;
    emac_cfg.smi_mdio_gpio_num = WESP_ETH_MDIO_GPIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns init failed"); return; }
    mdns_hostname_set(s_netcfg.hostname[0] ? s_netcfg.hostname : "wesp-probe");
    mdns_instance_name_set("wesp_debug_probe");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_instance_name_set("_http", "_tcp", "wESP32 debug probe");
    ESP_LOGI(TAG, "mdns: %s.local", s_netcfg.hostname);
}

/* OTA auto-confirm: if we got the network up + http running and stayed alive
 * for 30 s, mark this image valid so a power-cycle won't roll back. */
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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_net_evt = xEventGroupCreate();

    ethernet_init();
    ESP_ERROR_CHECK(uart_bridge_init_all());

    xEventGroupWaitBits(s_net_evt, NET_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    mdns_start();
    ESP_ERROR_CHECK(http_api_start());
    ota_arm_auto_confirm();
    ESP_LOGI(TAG, "wesp_debug_probe ready");
}
