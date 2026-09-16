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
    char last_error[32];
} sensor_reading_t;

#define SENSOR_DEBUG_EVENT_MAX 32

typedef struct {
    uint64_t time_ms;
    int channel;
    uint8_t addr;
    char dir[8];
    char op[32];
    char error[24];
} sensor_debug_event_t;

typedef struct {
    int freq_hz;
    int sda;
    int scl;
    uint8_t mux_addr;
    bool mux_present;
} sensor_i2c_info_t;

esp_err_t sensor_manager_init(void);
void sensor_manager_get(int channel, sensor_reading_t *out);
void sensor_manager_detect(void);
const char *sensor_family_name(sensor_family_t family);
void sensor_manager_i2c_info(sensor_i2c_info_t *out);
int sensor_manager_debug_copy(sensor_debug_event_t *out, int max);
void sensor_manager_debug_clear(void);
