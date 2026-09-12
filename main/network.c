#include "network.h"

#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "network";
static bool s_ap_mode;

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

static esp_err_t start_setup_ap(void)
{
    s_ap_mode = true;
    esp_netif_create_default_wifi_ap();

    wifi_config_t cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strcpy((char *)cfg.ap.ssid, APP_AP_SSID);
    cfg.ap.ssid_len = strlen(APP_AP_SSID);
    strcpy((char *)cfg.ap.password, APP_AP_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "setup AP %s at 192.168.4.1", APP_AP_SSID);
    return ESP_OK;
}

static esp_err_t start_station(const app_config_t *cfg)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    if (!cfg->dhcp) {
        esp_netif_dhcpc_stop(netif);

        esp_netif_ip_info_t ip = {0};
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(cfg->ip, &ip.ip));
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(cfg->gateway, &ip.gw));
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(cfg->netmask, &ip.netmask));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(cfg->dns, &dns.ip.u_addr.ip4));
        ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    }

    wifi_config_t wifi = {0};
    strncpy((char *)wifi.sta.ssid, cfg->ssid, sizeof(wifi.sta.ssid));
    strncpy((char *)wifi.sta.password, cfg->password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    return esp_wifi_start();
}

esp_err_t network_init(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events, NULL));

    if (!cfg.ssid[0]) {
        return start_setup_ap();
    }
    return start_station(&cfg);
}

int network_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_ap_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

const char *network_mode(void)
{
    return s_ap_mode ? "setup-ap" : "station";
}
