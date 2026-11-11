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
#include "lwip/sockets.h"
#include "network.h"
#include "ota.h"
#include "sensor_manager.h"

extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");
extern const unsigned char swagger_html_start[] asm("_binary_swagger_html_start");
extern const unsigned char swagger_html_end[] asm("_binary_swagger_html_end");
extern const unsigned char openapi_json_start[] asm("_binary_openapi_json_start");
extern const unsigned char openapi_json_end[] asm("_binary_openapi_json_end");
extern const unsigned char debug_html_start[] asm("_binary_debug_html_start");
extern const unsigned char debug_html_end[] asm("_binary_debug_html_end");

static const char *TAG = "web";

/*
 * With lru_purge_enable, a new connection beyond MAX_SOCKETS makes httpd close
 * the session whose last request is oldest (httpd_main.c, httpd_sess.c), on
 * every endpoint, so idle connections cannot lock other clients out.
 */
#define MAX_SOCKETS     8
#define KEEP_PER_CLIENT 2

static uint32_t s_closed_replies;

static int open_sockets(httpd_handle_t handle)
{
    int fds[MAX_SOCKETS];
    size_t n = MAX_SOCKETS;
    return httpd_get_client_list(handle, &n, fds) == ESP_OK ? (int)n : -1;
}

/*
 * The server listens on an IPv6 socket (CONFIG_LWIP_IPV6), so lwIP reports
 * IPv4 peers as ::ffff:a.b.c.d (lwIP sockets.c) and one compare covers both.
 */
static bool peer_key(int fd, uint8_t key[16])
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0 || addr.sin6_family != AF_INET6) {
        return false;
    }
    memcpy(key, &addr.sin6_addr, 16);
    return true;
}

/* Open connections from the same device as this request, including it. */
static int client_sockets(httpd_req_t *req)
{
    uint8_t mine[16];
    if (!peer_key(httpd_req_to_sockfd(req), mine)) {
        return -1;
    }
    int fds[MAX_SOCKETS];
    size_t n = MAX_SOCKETS;
    if (httpd_get_client_list(req->handle, &n, fds) != ESP_OK) {
        return -1;
    }
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t other[16];
        if (peer_key(fds[i], other) && memcmp(mine, other, 16) == 0) {
            count++;
        }
    }
    return count;
}

/*
 * A capture on the office unit showed Node-RED's HTTP request node (default
 * settings) leaving 12-14 connections open at a time while replies took up to
 * 14 s. A device keeps up to KEEP_PER_CLIENT connections; a JSON reply on any
 * further one closes it.
 */
static bool crowded(httpd_req_t *req)
{
    /* With this few open in total, no single device can be over its limit. */
    if (open_sockets(req->handle) <= KEEP_PER_CLIENT) {
        return false;
    }
    return client_sockets(req) > KEEP_PER_CLIENT;
}

static esp_err_t json_send(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const bool close = crowded(req);
    if (close) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);

    if (err == ESP_OK && close) {
        s_closed_replies++;
        /*
         * A handler error makes httpd delete the session right after this
         * handler returns (httpd_uri.c, httpd_main.c), with nothing queued
         * that could later hit a reused session slot.
         */
        return ESP_FAIL;
    }
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
    if (reading.last_error[0]) {
        cJSON_AddStringToObject(json, "lastError", reading.last_error);
    } else {
        cJSON_AddNullToObject(json, "lastError");
    }
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

static esp_err_t debug_page_get(httpd_req_t *req)
{
    return send_embedded(req, "text/html", debug_html_start, debug_html_end);
}

static void hex_addr(uint8_t addr, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    out[2] = hex[addr >> 4];
    out[3] = hex[addr & 0x0f];
    out[4] = '\0';
}

static esp_err_t debug_get(httpd_req_t *req)
{
    static sensor_debug_event_t events[SENSOR_DEBUG_EVENT_MAX];
    sensor_i2c_info_t i2c;
    sensor_manager_i2c_info(&i2c);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "firmwareVersion", APP_VERSION);
    cJSON_AddNumberToObject(json, "uptimeSeconds", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "uptimeMs", (double)(esp_timer_get_time() / 1000));

    cJSON *bus = cJSON_AddObjectToObject(json, "i2c");
    char mux_addr[5];
    hex_addr(i2c.mux_addr, mux_addr);
    cJSON_AddNumberToObject(bus, "freqHz", i2c.freq_hz);
    cJSON_AddNumberToObject(bus, "sda", i2c.sda);
    cJSON_AddNumberToObject(bus, "scl", i2c.scl);
    cJSON_AddStringToObject(bus, "muxAddress", mux_addr);
    cJSON_AddBoolToObject(bus, "muxPresent", i2c.mux_present);

    cJSON *channels = cJSON_AddArrayToObject(json, "channels");
    for (int i = 0; i < APP_CHANNELS; i++) {
        cJSON_AddItemToArray(channels, reading_json(i));
    }

    int n = sensor_manager_debug_copy(events, SENSOR_DEBUG_EVENT_MAX);
    cJSON *log = cJSON_AddArrayToObject(json, "events");
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "timeMs", (double)events[i].time_ms);
        cJSON_AddNumberToObject(item, "channel", events[i].channel);
        cJSON_AddNumberToObject(item, "addr", events[i].addr);
        cJSON_AddStringToObject(item, "dir", events[i].dir);
        cJSON_AddStringToObject(item, "op", events[i].op);
        cJSON_AddStringToObject(item, "error", events[i].error);
        cJSON_AddItemToArray(log, item);
    }
    return json_send(req, json);
}

static esp_err_t debug_clear(httpd_req_t *req)
{
    sensor_manager_debug_clear();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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
    network_wifi_status_t wifi;
    network_wifi_status(&wifi);
    cJSON_AddNumberToObject(json, "wifiRssi", wifi.rssi);
    cJSON_AddStringToObject(json, "wifiBssid", wifi.bssid);
    cJSON_AddNumberToObject(json, "wifiChannel", wifi.channel);
    cJSON_AddNumberToObject(json, "wifiDisconnects", wifi.disconnects);
    cJSON_AddNumberToObject(json, "wifiConnectFailures", wifi.connect_failures);
    cJSON_AddNumberToObject(json, "wifiLastDisconnectReason", wifi.last_reason);
    cJSON_AddNumberToObject(json, "wifiLastDisconnectUptime", wifi.last_disconnect_s);
    cJSON_AddStringToObject(json, "networkMode", network_mode());
    cJSON_AddStringToObject(json, "firmwarePartition", ota_running_partition());
    cJSON_AddBoolToObject(json, "firmwarePendingVerify", ota_pending_verify());
    cJSON_AddNumberToObject(json, "openSockets", open_sockets(req->handle));
    cJSON_AddNumberToObject(json, "clientSockets", client_sockets(req));
    cJSON_AddNumberToObject(json, "closedReplies", s_closed_replies);
    cJSON_AddNumberToObject(json, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "minFreeHeap", esp_get_minimum_free_heap_size());
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

#define ROAM_MARGIN_DB 6

static void ap_json(cJSON *parent, const char *name, const network_ap_t *ap)
{
    cJSON *o = cJSON_AddObjectToObject(parent, name);
    cJSON_AddStringToObject(o, "bssid", ap->bssid);
    cJSON_AddNumberToObject(o, "channel", ap->channel);
    cJSON_AddNumberToObject(o, "rssi", ap->rssi);
}

static void roam_task(void *arg)
{
    httpd_req_t *req = arg;
    network_roam_t r;
    const esp_err_t err = network_roam_scan(ROAM_MARGIN_DB, &r);

    if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_FINISHED) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, err == ESP_ERR_NOT_FINISHED ? "already running"
                                                            : "not connected to Wi-Fi");
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    } else {
        cJSON *json = cJSON_CreateObject();
        ap_json(json, "before", &r.before);
        ap_json(json, "best", &r.best);
        cJSON_AddNumberToObject(json, "apsFound", r.aps_found);
        cJSON_AddNumberToObject(json, "marginDb", ROAM_MARGIN_DB);
        cJSON_AddBoolToObject(json, "moving", r.moving);
        json_send(req, json);
    }
    httpd_req_async_handler_complete(req);

    if (r.moving) {
        /* Like reboot: let the reply leave before the link drops. */
        vTaskDelay(pdMS_TO_TICKS(200));
        network_roam_move();
    }
    vTaskDelete(NULL);
}

/* The scan takes a while: run it off the server task so other clients keep being served. */
static esp_err_t join_strongest_post(httpd_req_t *req)
{
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    if (xTaskCreate(roam_task, "roam", 6144, copy, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /*
     * Pollers (Node-RED) keep a keep-alive pool open, and clients on weak
     * Wi-Fi vanish without a FIN. Without these, dead sockets pile up until
     * every slot is taken and the device stops answering until a reboot.
     */
    cfg.max_open_sockets = MAX_SOCKETS;
    cfg.lru_purge_enable = true;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count = 3;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    /* json_send() closes crowded connections by returning ESP_FAIL, which
     * httpd_uri logs as a warning each time. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "start");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/swagger", .method = HTTP_GET, .handler = swagger_get},
        {.uri = "/debug", .method = HTTP_GET, .handler = debug_page_get},
        {.uri = "/api/docs", .method = HTTP_GET, .handler = swagger_get},
        {.uri = "/api/openapi.json", .method = HTTP_GET, .handler = openapi_get},
        {.uri = "/api/v1/debug", .method = HTTP_GET, .handler = debug_get},
        {.uri = "/api/v1/debug/clear", .method = HTTP_POST, .handler = debug_clear},
        {.uri = "/api/v1/sensors", .method = HTTP_GET, .handler = sensors_get},
        {.uri = "/api/v1/sensors/*", .method = HTTP_GET, .handler = sensor_get},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/v1/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/v1/config/network", .method = HTTP_PUT, .handler = network_put},
        {.uri = "/api/v1/config/channels", .method = HTTP_PUT, .handler = channels_put},
        {.uri = "/api/v1/sensors/detect", .method = HTTP_POST, .handler = action_post},
        {.uri = "/api/v1/reboot", .method = HTTP_POST, .handler = action_post},
        {.uri = "/api/v1/factory-reset", .method = HTTP_POST, .handler = action_post},
        {.uri = "/api/v1/firmware", .method = HTTP_POST, .handler = ota_upload_handler},
        {.uri = "/api/v1/wifi/join-strongest-ap", .method = HTTP_POST, .handler = join_strongest_post},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    return ESP_OK;
}
