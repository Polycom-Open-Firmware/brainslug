#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "probe.h"

static const char *TAG = "eth";

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "link up");   break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGW(TAG, "link down"); break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "start");     break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "stop");      break;
    }
}

#if CONFIG_PROBE_BOARD_WESP32
/* ---------- wESP32: internal EMAC + LAN8720 over RMII ---------- */
#include "esp_eth_mac_esp.h"

esp_netif_t *probe_eth_start(const net_cfg_t *netcfg)
{
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    net_cfg_apply(netif, netcfg);

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
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    return netif;
}

bool probe_pin_reserved(int g)
{
    if (g < 0 || g > 39) return true;
    if (g >= 6 && g <= 11) return true;          /* SPI flash */
    if (g == 16 || g == 17) return true;         /* MDC/MDIO */
    if (g == 0 || g == 18 || g == 19 || g == 21 ||
        g == 22 || g == 23 || g == 25 || g == 26 || g == 27) return true; /* RMII */
    return false;
}

#elif CONFIG_PROBE_BOARD_S3_POE_ETH_CAM
/* ---------- ESP32-S3 + W5500 over SPI ---------- */
#include "driver/spi_master.h"

esp_netif_t *probe_eth_start(const net_cfg_t *netcfg)
{
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t ncfg = { .base = &base_cfg, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    esp_netif_t *netif = esp_netif_new(&ncfg);
    net_cfg_apply(netif, netcfg);

    spi_bus_config_t bus = {
        .mosi_io_num = W5500_SPI_MOSI_GPIO,
        .miso_io_num = W5500_SPI_MISO_GPIO,
        .sclk_io_num = W5500_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(W5500_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* W5500 driver uses an interrupt on INT_GPIO; needs the ISR service. */
    gpio_install_isr_service(0);

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, NULL);
    spi_device_interface_config_t dev = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = W5500_SPI_CS_GPIO,
        .queue_size = 20,
    };
    w5500_cfg.int_gpio_num = W5500_INT_GPIO;
    w5500_cfg.spi_devcfg = &dev;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr        = W5500_PHY_ADDR;
    phy_cfg.reset_gpio_num  = W5500_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));

    /* W5500 has no factory MAC — derive one from the chip's eFuse. */
    uint8_t mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, mac_addr));

    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    return netif;
}

bool probe_pin_reserved(int g)
{
    /* ESP32-S3 has GPIOs 0..48. Strapping/USB pins are technically usable
     * but we keep the whitelist conservative on this board. */
    if (g < 0 || g > 48) return true;
    /* N16R8: SPI flash on 26-32, octal PSRAM on 33-37. */
    if (g >= 26 && g <= 37) return true;
    /* USB Serial JTAG D+/D-. */
    if (g == 19 || g == 20) return true;
    /* TF card slot (SPI3): CS=4, MISO=5, MOSI=6, SCK=7. */
    if (g >= 4 && g <= 7) return true;
    /* W5500 SPI + control. */
    if (g == W5500_SPI_MOSI_GPIO || g == W5500_SPI_MISO_GPIO ||
        g == W5500_SPI_SCLK_GPIO || g == W5500_SPI_CS_GPIO  ||
        g == W5500_INT_GPIO      || g == W5500_RST_GPIO) return true;
#if CONFIG_PROBE_CAMERA
    /* Camera bus. */
    int cam[] = { CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC,
                  CAM_PIN_D7, CAM_PIN_D6, CAM_PIN_D5, CAM_PIN_D4,
                  CAM_PIN_D3, CAM_PIN_D2, CAM_PIN_D1, CAM_PIN_D0,
                  CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK,
                  CAM_PIN_PWR_EN };
    for (size_t i = 0; i < sizeof cam / sizeof *cam; ++i)
        if (cam[i] >= 0 && cam[i] == g) return true;
#endif
    return false;
}
#endif
