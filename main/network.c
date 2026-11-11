#include "network.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "network";
static bool s_ap_mode;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_connected;
static uint32_t s_disconnects;
static uint32_t s_connect_failures;
static uint32_t s_last_reason;
static int32_t s_last_disconnect_s = -1;

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = data;
        wifi_ap_record_t ap;
        const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        portENTER_CRITICAL(&s_mux);
        s_connected = true;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "connected to %02X:%02X:%02X:%02X:%02X:%02X, channel %u, rssi %d",
                 e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
                 e->channel, rssi);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /*
         * Also raised for every failed esp_wifi_connect(). Only a loss of an
         * established link counts as a disconnect; failed attempts are
         * counted apart so they do not overwrite the reason of the drop.
         */
        const wifi_event_sta_disconnected_t *e = data;
        portENTER_CRITICAL(&s_mux);
        const bool was_connected = s_connected;
        s_connected = false;
        if (was_connected) {
            s_disconnects++;
            s_last_reason = e->reason;
            s_last_disconnect_s = (int32_t)(esp_timer_get_time() / 1000000);
        } else {
            s_connect_failures++;
        }
        portEXIT_CRITICAL(&s_mux);
        if (was_connected) {
            ESP_LOGW(TAG, "disconnected, reason %u, rssi %d", e->reason, e->rssi);
        } else {
            ESP_LOGI(TAG, "connect attempt failed, reason %u", e->reason);
        }
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
    /*
     * The default fast scan joins the first AP that matches the SSID. Scan
     * every channel and take the strongest, on each (re)connect.
     */
    wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    esp_err_t err = esp_wifi_start();
    /*
     * Modem sleep adds up to a beacon interval of latency per packet; on a
     * weak link that turns into retries and HTTP timeouts. USB-powered, so
     * keep the radio awake.
     */
    if (err == ESP_OK) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    return err;
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

void network_wifi_status(network_wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    wifi_ap_record_t ap;
    if (!s_ap_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
        out->channel = ap.primary;
        snprintf(out->bssid, sizeof(out->bssid), "%02X:%02X:%02X:%02X:%02X:%02X", ap.bssid[0],
                 ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
    portENTER_CRITICAL(&s_mux);
    out->disconnects = s_disconnects;
    out->connect_failures = s_connect_failures;
    out->last_reason = s_last_reason;
    out->last_disconnect_s = s_last_disconnect_s;
    portEXIT_CRITICAL(&s_mux);
}

const char *network_mode(void)
{
    return s_ap_mode ? "setup-ap" : "station";
}
