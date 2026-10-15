#include "ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "sdkconfig.h"

#define OTA_CHUNK       4096
#define OTA_RECV_RETRY  5

static const char *TAG = "ota";

bool ota_pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK
           && state == ESP_OTA_IMG_PENDING_VERIFY;
}

const char *ota_running_partition(void)
{
    return esp_ota_get_running_partition()->label;
}

static void mark_valid(void)
{
    if (ota_pending_verify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware %s on %s confirmed", esp_app_get_description()->version,
                 ota_running_partition());
    }
}

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    mark_valid();
}

esp_err_t ota_init(void)
{
    if (!ota_pending_verify()) {
        return ESP_OK;
    }
    /*
     * A new image only counts as good once it is reachable again. If it
     * crashes or is power-cycled before that, the bootloader reverts.
     */
    if (strcmp(network_mode(), "setup-ap") == 0) {
        mark_valid();
        return ESP_OK;
    }
    ESP_LOGW(TAG, "new firmware pending: confirming after Wi-Fi gets an IP");
    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL);

    /* The IP may have arrived before the handler was registered. */
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_is_netif_up(sta)
        && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        mark_valid();
    }
    return err;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGW(TAG, "upload rejected: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static bool token_ok(httpd_req_t *req)
{
    char token[65] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return strcmp(token, CONFIG_ENVIRO8_OTA_TOKEN) == 0;
}

/* Rejects images built for another project before anything is erased. */
static bool image_is_enviro8(const uint8_t *buf, int len)
{
    const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < (int)(off + sizeof(esp_app_desc_t))) {
        return false;
    }
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + off);
    return desc->magic_word == ESP_APP_DESC_MAGIC_WORD
           && strcmp(desc->project_name, esp_app_get_description()->project_name) == 0;
}

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (CONFIG_ENVIRO8_OTA_TOKEN[0] == '\0') {
        return fail(req, "403 Forbidden", "network update disabled: firmware built without an OTA token");
    }
    if (!token_ok(req)) {
        return fail(req, "401 Unauthorized", "missing or wrong X-OTA-Token");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return fail(req, "500 Internal Server Error", "no OTA partition: flash the OTA partition table over USB");
    }
    if (req->content_len <= 0 || req->content_len > target->size) {
        return fail(req, "400 Bad Request", "image size invalid or larger than the OTA slot");
    }

    static uint8_t buf[OTA_CHUNK];
    esp_ota_handle_t ota = 0;
    int remaining = req->content_len;
    int retries = 0;
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, target->label);
    while (remaining > 0) {
        /* Fill whole chunks so the first one always holds the app descriptor. */
        int want = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int n = 0;
        while (n < want) {
            int got = httpd_req_recv(req, (char *)buf + n, want - n);
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= OTA_RECV_RETRY) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            retries = 0;
            n += got;
        }
        if (n < want) {
            err = ESP_FAIL;
            break;
        }

        if (!ota) {
            if (!image_is_enviro8(buf, n)) {
                return fail(req, "400 Bad Request", "not an Enviro8 firmware image");
            }
            err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) {
                break;
            }
        }
        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            break;
        }
        remaining -= n;
    }

    if (err != ESP_OK || remaining > 0) {
        if (ota) {
            esp_ota_abort(ota);
        }
        return fail(req, "500 Internal Server Error", "upload interrupted or flash write failed");
    }

    err = esp_ota_end(ota);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        return fail(req, "400 Bad Request", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "update written to %s, rebooting", target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
