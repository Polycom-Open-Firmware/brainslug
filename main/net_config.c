#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "probe.h"

static const char *TAG = "net-cfg";

esp_err_t net_cfg_load(net_cfg_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode = NET_DHCP;
    strcpy(out->hostname, "wesp-probe");

    nvs_handle_t h;
    if (nvs_open("probe", NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    size_t sz = sizeof *out;
    nvs_get_blob(h, "net", out, &sz);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t net_cfg_save(const net_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("probe", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_blob(h, "net", cfg, sizeof *cfg);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

void net_cfg_apply(esp_netif_t *netif, const net_cfg_t *cfg)
{
    if (cfg->hostname[0]) esp_netif_set_hostname(netif, cfg->hostname);

    if (cfg->mode == NET_STATIC && cfg->ip[0] && cfg->netmask[0]) {
        esp_netif_dhcpc_stop(netif);
        esp_netif_ip_info_t info = {0};
        info.ip.addr      = ipaddr_addr(cfg->ip);
        info.netmask.addr = ipaddr_addr(cfg->netmask);
        if (cfg->gw[0]) info.gw.addr = ipaddr_addr(cfg->gw);
        esp_netif_set_ip_info(netif, &info);
        if (cfg->dns[0]) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.u_addr.ip4.addr = ipaddr_addr(cfg->dns);
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "static %s/%s gw=%s", cfg->ip, cfg->netmask, cfg->gw);
    } else {
        ESP_LOGI(TAG, "dhcp, hostname=%s", cfg->hostname);
    }
}
