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
static uint32_t s_roams;
static uint32_t s_last_reason;
static int32_t s_last_disconnect_s = -1;

#define ROAM_MAX_APS  16
#define ROAM_RECOVER_S 45 /* > two 10 s handshake timeouts plus scans */
/* join-strongest-ap: set while we leave on purpose to join a chosen BSSID. */
static bool s_roam_leaving;
/* The station config is locked to one BSSID; undone on the next drop. */
static bool s_pinned;
static bool s_roam_busy;
static wifi_config_t s_roam_cfg; /* station config locked to the chosen BSSID */

/* Back to the normal strongest-AP join. Stays pinned (retried on the next drop) if it fails. */
static void unpin(void)
{
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) {
        return;
    }
    c.sta.bssid_set = false;
    c.sta.channel = 0;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &c);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_pinned = false;
        portEXIT_CRITICAL(&s_mux);
    } else {
        ESP_LOGW(TAG, "unpin failed: %s", esp_err_to_name(err));
    }
}

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
        /*
         * Our own leave reports ASSOC_LEAVE (wifi.rst, reason codes). Any other
         * reason is a real drop, even while a move is pending; the flag then
         * stays for the leave's own event.
         */
        const bool roaming = s_roam_leaving && e->reason == WIFI_REASON_ASSOC_LEAVE;
        const bool pinned = s_pinned;
        s_connected = false;
        if (roaming) {
            s_roam_leaving = false;
        }
        if (was_connected && roaming) {
            s_roams++; /* our own leave: not a link loss */
        } else if (was_connected) {
            s_disconnects++;
            s_last_reason = e->reason;
            s_last_disconnect_s = (int32_t)(esp_timer_get_time() / 1000000);
        } else {
            s_connect_failures++;
        }
        portEXIT_CRITICAL(&s_mux);
        if (was_connected && roaming) {
            ESP_LOGI(TAG, "left the AP to roam");
        } else if (was_connected) {
            ESP_LOGW(TAG, "disconnected, reason %u, rssi %d", e->reason, e->rssi);
        } else {
            ESP_LOGI(TAG, "connect attempt failed, reason %u", e->reason);
        }
        /*
         * The connect that follows our own leave keeps the BSSID lock; any
         * other drop, or a failed join of the chosen AP, goes back to the
         * normal strongest-AP scan.
         */
        if (!roaming && pinned) {
            unpin();
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
    out->roams = s_roams;
    out->last_reason = s_last_reason;
    out->last_disconnect_s = s_last_disconnect_s;
    portEXIT_CRITICAL(&s_mux);
}

static void fill_ap(network_ap_t *out, const uint8_t *bssid, int channel, int rssi)
{
    snprintf(out->bssid, sizeof(out->bssid), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0],
             bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    out->channel = channel;
    out->rssi = rssi;
}

static void roam_release(void)
{
    portENTER_CRITICAL(&s_mux);
    s_roam_busy = false;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t network_roam_scan(int margin_db, network_roam_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_ap_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_roam_busy;
    s_roam_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        return ESP_ERR_NOT_FINISHED;
    }

    esp_err_t err = ESP_OK;
    /* One roam at a time (s_roam_busy), so one static list is enough. */
    static wifi_ap_record_t recs[ROAM_MAX_APS];
    wifi_ap_record_t cur;
    wifi_config_t cfg;
    if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK
        || esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    fill_ap(&out->before, cur.bssid, cur.primary, cur.rssi);

    /* Background scan: the link stays up; the radio returns to its channel between channels. */
    static uint8_t ssid[sizeof(cfg.sta.ssid) + 1];
    memcpy(ssid, cfg.sta.ssid, sizeof(cfg.sta.ssid));
    ssid[sizeof(cfg.sta.ssid)] = 0;
    wifi_scan_config_t scan = {
        .ssid = ssid,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        goto done;
    }
    uint16_t n = ROAM_MAX_APS;
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list(); /* required after a failed read (esp_wifi.h) */
        goto done;
    }
    out->aps_found = n;

    int best = -1;
    int cur_rssi = cur.rssi;
    for (int i = 0; i < n; i++) {
        if (best < 0 || recs[i].rssi > recs[best].rssi) {
            best = i;
        }
        if (memcmp(recs[i].bssid, cur.bssid, sizeof(cur.bssid)) == 0) {
            cur_rssi = recs[i].rssi; /* same measurement time as the others */
        }
    }
    out->before.rssi = cur_rssi;
    if (best < 0) {
        out->best = out->before;
        goto done;
    }
    fill_ap(&out->best, recs[best].bssid, recs[best].primary, recs[best].rssi);

    if (memcmp(recs[best].bssid, cur.bssid, sizeof(cur.bssid)) != 0
        && recs[best].rssi >= cur_rssi + margin_db) {
        s_roam_cfg = cfg;
        s_roam_cfg.sta.bssid_set = true;
        memcpy(s_roam_cfg.sta.bssid, recs[best].bssid, sizeof(s_roam_cfg.sta.bssid));
        s_roam_cfg.sta.channel = recs[best].primary;
        /* Stop at the chosen BSSID (scan starts on its channel). */
        s_roam_cfg.sta.scan_method = WIFI_FAST_SCAN;
        out->moving = true;
    }

done:
    if (!out->moving) {
        roam_release();
    }
    return err;
}

bool network_connected(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool c = s_connected;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

esp_err_t network_roam_move(void)
{
    /* The link may have dropped since the scan; the auto-reconnect owns it then. */
    wifi_ap_record_t cur;
    if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) {
        ESP_LOGW(TAG, "roam cancelled: not connected any more");
        roam_release();
        return ESP_ERR_INVALID_STATE;
    }
    if (memcmp(cur.bssid, s_roam_cfg.sta.bssid, sizeof(cur.bssid)) == 0) {
        ESP_LOGI(TAG, "roam skipped: already on the target AP");
        roam_release();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "moving to %02X:%02X:%02X:%02X:%02X:%02X ch %u", s_roam_cfg.sta.bssid[0],
             s_roam_cfg.sta.bssid[1], s_roam_cfg.sta.bssid[2], s_roam_cfg.sta.bssid[3],
             s_roam_cfg.sta.bssid[4], s_roam_cfg.sta.bssid[5], s_roam_cfg.sta.channel);
    portENTER_CRITICAL(&s_mux);
    s_roam_leaving = true;
    s_pinned = true;
    portEXIT_CRITICAL(&s_mux);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_roam_cfg);
    if (err == ESP_OK) {
        err = esp_wifi_disconnect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "roam failed: %s", esp_err_to_name(err));
        portENTER_CRITICAL(&s_mux);
        s_roam_leaving = false;
        portEXIT_CRITICAL(&s_mux);
        unpin();
        roam_release();
        return err;
    }

    /*
     * Only the DISCONNECTED handler restarts the connection. If our leave
     * cancelled a reconnect already under way and no event follows, nothing
     * would; after ROAM_RECOVER_S without a link, clear the move and connect.
     */
    for (int i = 0; i < ROAM_RECOVER_S && !network_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!network_connected()) {
        portENTER_CRITICAL(&s_mux);
        s_roam_leaving = false;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "no link %d s after the move, reconnecting", ROAM_RECOVER_S);
        if (s_pinned) {
            unpin();
        }
        esp_wifi_connect();
    }
    roam_release();
    return ESP_OK;
}

const char *network_mode(void)
{
    return s_ap_mode ? "setup-ap" : "station";
}
