#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "sensor_manager.h"

extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");
extern const unsigned char swagger_html_start[] asm("_binary_swagger_html_start");
extern const unsigned char swagger_html_end[] asm("_binary_swagger_html_end");
extern const unsigned char openapi_json_start[] asm("_binary_openapi_json_start");
extern const unsigned char openapi_json_end[] asm("_binary_openapi_json_end");

static const char *TAG = "web";

static esp_err_t json_send(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    cJSON_Delete(json);
    return err;
}

static cJSON *reading_json(int channel)
{
    sensor_reading_t reading;
    sensor_manager_get(channel, &reading);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "channel", channel + 1);
    cJSON_AddBoolToObject(json, "online", reading.online);
    cJSON_AddStringToObject(json, "configuredType", sensor_type_name(reading.configured));
    cJSON_AddStringToObject(json, "detectedFamily", sensor_family_name(reading.family));

    if (reading.serial[0]) {
        cJSON_AddStringToObject(json, "serialNumber", reading.serial);
    } else {
        cJSON_AddNullToObject(json, "serialNumber");
    }

    if (reading.online) {
        cJSON_AddNumberToObject(json, "rawTemperature", reading.raw_temperature);
        cJSON_AddNumberToObject(json, "rawHumidity", reading.raw_humidity);
        cJSON_AddNumberToObject(json, "temperature", reading.temperature);
        cJSON_AddNumberToObject(json, "humidity", reading.humidity);
    } else {
        cJSON_AddNullToObject(json, "rawTemperature");
        cJSON_AddNullToObject(json, "rawHumidity");
        cJSON_AddNullToObject(json, "temperature");
        cJSON_AddNullToObject(json, "humidity");
    }

    cJSON_AddNumberToObject(json, "lastReadMs", (double)reading.last_read_ms);
    cJSON_AddNumberToObject(json, "errorCount", reading.error_count);
    cJSON_AddNumberToObject(json, "sampleCount", reading.sample_count);
    return json;
}

static esp_err_t send_embedded(httpd_req_t *req, const char *type,
                               const unsigned char *start, const unsigned char *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t index_get(httpd_req_t *req)
{
    return send_embedded(req, "text/html", index_html_start, index_html_end);
}

static esp_err_t swagger_get(httpd_req_t *req)
{
    return send_embedded(req, "text/html", swagger_html_start, swagger_html_end);
}

static esp_err_t openapi_get(httpd_req_t *req)
{
    return send_embedded(req, "application/json", openapi_json_start, openapi_json_end);
}

static esp_err_t sensors_get(httpd_req_t *req)
{
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < APP_CHANNELS; i++) {
        cJSON_AddItemToArray(array, reading_json(i));
    }
    return json_send(req, array);
}

static esp_err_t sensor_get(httpd_req_t *req)
{
    const char *slash = strrchr(req->uri, '/');
    int channel = slash ? atoi(slash + 1) : 0;
    if (channel < 1 || channel > APP_CHANNELS) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "channel must be 1-8");
        return ESP_OK;
    }
    return json_send(req, reading_json(channel - 1));
}

static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "firmwareVersion", APP_VERSION);
    cJSON_AddNumberToObject(json, "uptimeSeconds", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "wifiRssi", network_rssi());
    cJSON_AddStringToObject(json, "networkMode", network_mode());
    return json_send(req, json);
}

static cJSON *config_json(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *json = cJSON_CreateObject();
    cJSON *network = cJSON_AddObjectToObject(json, "network");
    cJSON *channels = cJSON_AddArrayToObject(json, "channels");

    cJSON_AddBoolToObject(network, "dhcp", cfg.dhcp);
    cJSON_AddStringToObject(network, "ssid", cfg.ssid);
    cJSON_AddStringToObject(network, "ip", cfg.ip);
    cJSON_AddStringToObject(network, "gateway", cfg.gateway);
    cJSON_AddStringToObject(network, "netmask", cfg.netmask);
    cJSON_AddStringToObject(network, "dns", cfg.dns);

    for (int i = 0; i < APP_CHANNELS; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "channel", i + 1);
        cJSON_AddStringToObject(item, "type", sensor_type_name(cfg.channels[i].type));
        cJSON_AddNumberToObject(item, "temperatureOffset", cfg.channels[i].temp_offset);
        cJSON_AddNumberToObject(item, "humidityOffset", cfg.channels[i].humidity_offset);
        cJSON_AddItemToArray(channels, item);
    }
    return json;
}

static esp_err_t config_get(httpd_req_t *req)
{
    return json_send(req, config_json());
}

static cJSON *body_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        return NULL;
    }

    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) {
        return NULL;
    }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        return NULL;
    }

    buf[received] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static void copy_string(cJSON *json, const char *key, char *dst, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsString(item)) {
        strncpy(dst, item->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

static esp_err_t network_put(httpd_req_t *req)
{
    cJSON *json = body_json(req);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *dhcp = cJSON_GetObjectItem(json, "dhcp");
    if (cJSON_IsBool(dhcp)) {
        cfg.dhcp = cJSON_IsTrue(dhcp);
    }

    copy_string(json, "ssid", cfg.ssid, sizeof(cfg.ssid));
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (cJSON_IsString(password) && password->valuestring[0]) {
        copy_string(json, "password", cfg.password, sizeof(cfg.password));
    }
    copy_string(json, "ip", cfg.ip, sizeof(cfg.ip));
    copy_string(json, "gateway", cfg.gateway, sizeof(cfg.gateway));
    copy_string(json, "netmask", cfg.netmask, sizeof(cfg.netmask));
    copy_string(json, "dns", cfg.dns, sizeof(cfg.dns));
    cJSON_Delete(json);

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_OK;
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddBoolToObject(ok, "rebootRequired", true);
    return json_send(req, ok);
}

static esp_err_t channels_put(httpd_req_t *req)
{
    cJSON *json = body_json(req);
    cJSON *array = cJSON_IsArray(json) ? json : cJSON_GetObjectItem(json, "channels");
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected channels array");
        return ESP_OK;
    }

    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        cJSON *channel = cJSON_GetObjectItem(item, "channel");
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *temp_offset = cJSON_GetObjectItem(item, "temperatureOffset");
        cJSON *humidity_offset = cJSON_GetObjectItem(item, "humidityOffset");

        if (!cJSON_IsNumber(channel) || channel->valueint < 1 || channel->valueint > APP_CHANNELS
            || !cJSON_IsString(type)) {
            continue;
        }

        int index = channel->valueint - 1;
        cfg.channels[index].type = sensor_type_parse(type->valuestring);
        if (cJSON_IsNumber(temp_offset)) {
            cfg.channels[index].temp_offset = (float)temp_offset->valuedouble;
        }
        if (cJSON_IsNumber(humidity_offset)) {
            cfg.channels[index].humidity_offset = (float)humidity_offset->valuedouble;
        }
    }

    cJSON_Delete(json);
    app_config_save(&cfg);
    sensor_manager_detect();
    return json_send(req, config_json());
}

static esp_err_t action_post(httpd_req_t *req)
{
    if (strstr(req->uri, "detect")) {
        sensor_manager_detect();
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (strstr(req->uri, "factory-reset")) {
        app_config_factory_reset();
        httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "start");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/swagger", .method = HTTP_GET, .handler = swagger_get},
        {.uri = "/api/docs", .method = HTTP_GET, .handler = swagger_get},
        {.uri = "/api/openapi.json", .method = HTTP_GET, .handler = openapi_get},
        {.uri = "/api/v1/sensors", .method = HTTP_GET, .handler = sensors_get},
        {.uri = "/api/v1/sensors/*", .method = HTTP_GET, .handler = sensor_get},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/v1/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/v1/config/network", .method = HTTP_PUT, .handler = network_put},
        {.uri = "/api/v1/config/channels", .method = HTTP_PUT, .handler = channels_put},
        {.uri = "/api/v1/sensors/detect", .method = HTTP_POST, .handler = action_post},
        {.uri = "/api/v1/reboot", .method = HTTP_POST, .handler = action_post},
        {.uri = "/api/v1/factory-reset", .method = HTTP_POST, .handler = action_post},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    return ESP_OK;
}
