#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef enum {
    FAMILY_NONE = 0,
    FAMILY_SHT4X,
    FAMILY_SHT3X,
    FAMILY_SHT2X,
    FAMILY_OTHER,
} sensor_family_t;

typedef struct {
    bool online;
    sensor_type_t configured;
    sensor_family_t family;
    float raw_temperature;
    float raw_humidity;
    float temperature;
    float humidity;
    uint64_t last_read_ms;
    uint32_t error_count;
    uint32_t sample_count;
    char serial[24];
} sensor_reading_t;

esp_err_t sensor_manager_init(void);
void sensor_manager_get(int channel, sensor_reading_t *out);
void sensor_manager_detect(void);
const char *sensor_family_name(sensor_family_t family);
