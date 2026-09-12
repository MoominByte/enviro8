#include "app_config.h"

#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static app_config_t s_cfg;
static SemaphoreHandle_t s_lock;

static const char *s_type_names[] = {
    "Disabled",
    "Auto",
    "SHT45",
    "SHT41",
    "SHT40",
    "SHT35",
    "SHT31",
    "SHT30",
    "SHT20",
    "Other",
};

static void set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->dhcp = true;
    strcpy(cfg->ip, "192.168.1.50");
    strcpy(cfg->gateway, "192.168.1.1");
    strcpy(cfg->netmask, "255.255.255.0");
    strcpy(cfg->dns, "8.8.8.8");
    for (int i = 0; i < APP_CHANNELS; i++) {
        cfg->channels[i].type = SENSOR_AUTO;
    }
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    set_defaults(&s_cfg);

    nvs_handle_t handle = 0;
    if (nvs_open("app", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(s_cfg);
        err = nvs_get_blob(handle, "config", &s_cfg, &size);
        nvs_close(handle);
        if (err != ESP_OK || size != sizeof(s_cfg)) {
            set_defaults(&s_cfg);
        }
    }
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("app", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "config", cfg, sizeof(*cfg));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle) {
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *cfg;
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t app_config_factory_reset(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("app", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle) {
        nvs_close(handle);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_defaults(&s_cfg);
    xSemaphoreGive(s_lock);
    return err;
}

const char *sensor_type_name(sensor_type_t type)
{
    if (type >= SENSOR_DISABLED && type <= SENSOR_OTHER) {
        return s_type_names[type];
    }
    return "Other";
}

sensor_type_t sensor_type_parse(const char *name)
{
    for (int i = 0; i <= SENSOR_OTHER; i++) {
        if (strcasecmp(name, s_type_names[i]) == 0) {
            return (sensor_type_t)i;
        }
    }
    return SENSOR_OTHER;
}
