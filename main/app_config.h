#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_VERSION "1.0.0"
#define APP_AP_SSID "Enviro8-Setup"
#define APP_AP_PASSWORD "configureme"
#define APP_CHANNELS 8
#define APP_STR_LEN 33

typedef enum { SENSOR_DISABLED, SENSOR_AUTO, SENSOR_SHT45, SENSOR_SHT41, SENSOR_SHT40,
    SENSOR_SHT35, SENSOR_SHT31, SENSOR_SHT30, SENSOR_SHT20, SENSOR_OTHER } sensor_type_t;

typedef struct { sensor_type_t type; float temp_offset; float humidity_offset; } channel_config_t;
typedef struct {
    bool dhcp;
    char ssid[APP_STR_LEN];
    char password[65];
    char ip[16], gateway[16], netmask[16], dns[16];
    channel_config_t channels[APP_CHANNELS];
} app_config_t;

esp_err_t app_config_init(void);
void app_config_get(app_config_t *out);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_factory_reset(void);
const char *sensor_type_name(sensor_type_t type);
sensor_type_t sensor_type_parse(const char *name);
