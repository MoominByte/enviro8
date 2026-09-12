#include "sensor_manager.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define I2C_PORT           I2C_NUM_0
#define I2C_SDA            GPIO_NUM_22
#define I2C_SCL            GPIO_NUM_23
#define I2C_FREQ_HZ        400000
#define I2C_TIMEOUT_MS     200
#define POLL_INTERVAL_MS   1000
#define TCA_ADDR           0x70
#define SHT_ADDR_A         0x44
#define SHT_ADDR_B         0x45
#define SHT2X_ADDR         0x40

static const char *TAG = "sensors";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_mux;
static i2c_master_dev_handle_t s_dev_44;
static i2c_master_dev_handle_t s_dev_45;
static i2c_master_dev_handle_t s_dev_40;
static sensor_reading_t s_readings[APP_CHANNELS];
static SemaphoreHandle_t s_lock;
static volatile bool s_detect_requested = true;

static esp_err_t i2c_tx(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len)
{
    return i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t i2c_rx(i2c_master_dev_handle_t dev, uint8_t *data, size_t len)
{
    return i2c_master_receive(dev, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ_HZ,
        .scl_wait_us = 20000,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, dev);
}

static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool crc_ok(const uint8_t *frame)
{
    return crc8(frame, 2) == frame[2] && crc8(frame + 3, 2) == frame[5];
}

static void bus_recover(void)
{
    i2c_master_bus_reset(s_bus);
    uint8_t off = 0;
    i2c_tx(s_mux, &off, 1);
}

static esp_err_t mux_select(int channel)
{
    uint8_t mask = (uint8_t)(1u << channel);
    esp_err_t err = i2c_tx(s_mux, &mask, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mux select ch%d failed (%s), resetting bus", channel, esp_err_to_name(err));
        bus_recover();
        err = i2c_tx(s_mux, &mask, 1);
    }
    /* Analog switch needs a moment before the downstream device is addressed. */
    vTaskDelay(pdMS_TO_TICKS(2));
    return err;
}

static void mux_disable(void)
{
    uint8_t off = 0;
    if (i2c_tx(s_mux, &off, 1) != ESP_OK) {
        bus_recover();
    }
}

static bool probe_addr(uint8_t addr)
{
    return i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) == ESP_OK;
}

static i2c_master_dev_handle_t handle_for_addr(uint8_t addr)
{
    if (addr == SHT_ADDR_A) {
        return s_dev_44;
    }
    if (addr == SHT_ADDR_B) {
        return s_dev_45;
    }
    return s_dev_40;
}

static void sht3x_recover(i2c_master_dev_handle_t dev)
{
    const uint8_t brk[] = {0x30, 0x93};
    const uint8_t rst[] = {0x30, 0xA2};
    i2c_tx(dev, brk, sizeof(brk));
    vTaskDelay(pdMS_TO_TICKS(1));
    i2c_tx(dev, rst, sizeof(rst));
    vTaskDelay(pdMS_TO_TICKS(2));
}

static esp_err_t read_sht4x(i2c_master_dev_handle_t dev, float *temp, float *humidity, char *serial)
{
    uint8_t cmd = 0x89;
    uint8_t raw[6];
    esp_err_t err = i2c_tx(dev, &cmd, 1);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err == ESP_OK && crc_ok(raw)) {
        snprintf(serial, 24, "%02X%02X%02X%02X", raw[0], raw[1], raw[3], raw[4]);
    } else if (err != ESP_OK) {
        return err;
    } else {
        return ESP_ERR_INVALID_CRC;
    }

    cmd = 0xFD;
    err = i2c_tx(dev, &cmd, 1);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err != ESP_OK || !crc_ok(raw)) {
        return err ? err : ESP_ERR_INVALID_CRC;
    }

    uint16_t t_raw = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t h_raw = ((uint16_t)raw[3] << 8) | raw[4];
    *temp = -45.0f + 175.0f * (t_raw / 65535.0f);
    *humidity = -6.0f + 125.0f * (h_raw / 65535.0f);
    return ESP_OK;
}

static esp_err_t read_sht3x(i2c_master_dev_handle_t dev, float *temp, float *humidity)
{
    const uint8_t cmd[] = {0x24, 0x00};
    uint8_t raw[6];

    esp_err_t err = i2c_tx(dev, cmd, sizeof(cmd));
    if (err == ESP_OK) {
        /* SHT35 high-repeatability conversion is 12.5 ms typ / 15.5 ms max. */
        vTaskDelay(pdMS_TO_TICKS(20));
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err != ESP_OK || !crc_ok(raw)) {
        return err ? err : ESP_ERR_INVALID_CRC;
    }

    uint16_t t_raw = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t h_raw = ((uint16_t)raw[3] << 8) | raw[4];
    *temp = -45.0f + 175.0f * (t_raw / 65535.0f);
    *humidity = 100.0f * (h_raw / 65535.0f);
    return ESP_OK;
}

static esp_err_t read_sht2x(i2c_master_dev_handle_t dev, float *temp, float *humidity)
{
    uint8_t cmd = 0xF3;
    uint8_t raw[3];
    esp_err_t err = i2c_tx(dev, &cmd, 1);
    vTaskDelay(pdMS_TO_TICKS(90));
    if (err == ESP_OK) {
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err == ESP_OK) {
        uint16_t t_raw = (((uint16_t)raw[0] << 8) | raw[1]) & ~0x3;
        *temp = -46.85f + 175.72f * t_raw / 65536.0f;
        cmd = 0xF5;
        err = i2c_tx(dev, &cmd, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (err == ESP_OK) {
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err != ESP_OK) {
        return err;
    }

    uint16_t h_raw = (((uint16_t)raw[0] << 8) | raw[1]) & ~0x3;
    *humidity = -6.0f + 125.0f * h_raw / 65536.0f;
    return ESP_OK;
}

static sensor_family_t family_from_type(sensor_type_t type)
{
    if (type >= SENSOR_SHT45 && type <= SENSOR_SHT40) {
        return FAMILY_SHT4X;
    }
    if (type >= SENSOR_SHT35 && type <= SENSOR_SHT30) {
        return FAMILY_SHT3X;
    }
    if (type == SENSOR_SHT20) {
        return FAMILY_SHT2X;
    }
    if (type == SENSOR_OTHER) {
        return FAMILY_OTHER;
    }
    return FAMILY_NONE;
}

static uint8_t first_present(const uint8_t *addrs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (probe_addr(addrs[i])) {
            return addrs[i];
        }
    }
    return 0;
}

static esp_err_t sample_channel(int channel, sensor_type_t type, sensor_reading_t *out, bool detect)
{
    if (type == SENSOR_DISABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = mux_select(channel);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t sht_addrs[] = {SHT_ADDR_A, SHT_ADDR_B};
    const uint8_t addr_sht = first_present(sht_addrs, sizeof(sht_addrs));
    const bool has_sht2x = probe_addr(SHT2X_ADDR);
    const bool auto_or_detect = detect || type == SENSOR_AUTO;
    sensor_family_t family = family_from_type(type);
    float temp = NAN;
    float humidity = NAN;
    char serial[24] = {0};

    ESP_LOGD(TAG, "ch%d scan 0x44=%d 0x45=%d 0x40=%d",
             channel, probe_addr(SHT_ADDR_A), probe_addr(SHT_ADDR_B), has_sht2x);

    if (auto_or_detect) {
        /*
         * Probe SHT3x before SHT4x. Both families share 0x44/0x45, and an
         * SHT4x serial/measure sequence leaves an SHT35 unable to answer.
         */
        if (addr_sht) {
            i2c_master_dev_handle_t dev = handle_for_addr(addr_sht);
            if (detect) {
                sht3x_recover(dev);
            }
            if (read_sht3x(dev, &temp, &humidity) == ESP_OK) {
                family = FAMILY_SHT3X;
                err = ESP_OK;
            } else if (read_sht4x(dev, &temp, &humidity, serial) == ESP_OK) {
                family = FAMILY_SHT4X;
                err = ESP_OK;
            } else {
                err = ESP_ERR_NOT_FOUND;
            }
        } else if (has_sht2x && read_sht2x(s_dev_40, &temp, &humidity) == ESP_OK) {
            family = FAMILY_SHT2X;
            err = ESP_OK;
        } else {
            err = ESP_ERR_NOT_FOUND;
        }
    } else if (family == FAMILY_SHT4X || family == FAMILY_SHT3X) {
        uint8_t addr = addr_sht ? addr_sht : SHT_ADDR_A;
        i2c_master_dev_handle_t dev = handle_for_addr(addr);
        err = (family == FAMILY_SHT4X)
                  ? read_sht4x(dev, &temp, &humidity, serial)
                  : read_sht3x(dev, &temp, &humidity);
    } else if (family == FAMILY_SHT2X) {
        err = read_sht2x(s_dev_40, &temp, &humidity);
    } else {
        err = ESP_ERR_NOT_FOUND;
    }

    mux_disable();

    if (!isnan(temp)) {
        out->family = family;
        out->raw_temperature = temp;
        out->raw_humidity = humidity;
        if (serial[0]) {
            strncpy(out->serial, serial, sizeof(out->serial) - 1);
            out->serial[sizeof(out->serial) - 1] = '\0';
        }
        return ESP_OK;
    }

    if (err != ESP_OK) {
        bus_recover();
    }
    return err;
}

static void poll_task(void *arg)
{
    (void)arg;

    while (1) {
        app_config_t cfg;
        app_config_get(&cfg);

        const bool detect = s_detect_requested;
        s_detect_requested = false;

        for (int i = 0; i < APP_CHANNELS; i++) {
            sensor_reading_t reading;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            reading = s_readings[i];
            xSemaphoreGive(s_lock);

            reading.configured = cfg.channels[i].type;
            esp_err_t err = sample_channel(i, reading.configured, &reading, detect);
            reading.online = (err == ESP_OK);

            if (err == ESP_OK) {
                reading.temperature = reading.raw_temperature + cfg.channels[i].temp_offset;
                reading.humidity = reading.raw_humidity + cfg.channels[i].humidity_offset;
                reading.last_read_ms = (uint64_t)(esp_timer_get_time() / 1000);
                reading.sample_count++;
                if (detect) {
                    ESP_LOGI(TAG, "ch%d detected %s T=%.2f H=%.2f",
                             i, sensor_family_name(reading.family),
                             reading.raw_temperature, reading.raw_humidity);
                }
            } else if (err != ESP_ERR_NOT_SUPPORTED) {
                reading.error_count++;
                if (detect) {
                    ESP_LOGW(TAG, "ch%d detect failed: %s", i, esp_err_to_name(err));
                } else {
                    ESP_LOGD(TAG, "ch%d sample failed: %s", i, esp_err_to_name(err));
                }
            }

            if (reading.configured == SENSOR_DISABLED) {
                reading.online = false;
                reading.family = FAMILY_NONE;
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_readings[i] = reading;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t sensor_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "bus");
    ESP_RETURN_ON_ERROR(add_dev(TCA_ADDR, &s_mux), TAG, "mux");
    ESP_RETURN_ON_ERROR(add_dev(SHT_ADDR_A, &s_dev_44), TAG, "0x44");
    ESP_RETURN_ON_ERROR(add_dev(SHT_ADDR_B, &s_dev_45), TAG, "0x45");
    ESP_RETURN_ON_ERROR(add_dev(SHT2X_ADDR, &s_dev_40), TAG, "0x40");

    if (!probe_addr(TCA_ADDR)) {
        ESP_LOGW(TAG, "TCA9548A not found at 0x%02X — check SDA/SCL, A0-A2 and RESET", TCA_ADDR);
    } else {
        ESP_LOGI(TAG, "I2C ready at %d Hz, mux at 0x%02X", I2C_FREQ_HZ, TCA_ADDR);
        mux_disable();
    }

    xTaskCreate(poll_task, "sensor_poll", 6144, NULL, 5, NULL);
    return ESP_OK;
}

void sensor_manager_get(int channel, sensor_reading_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_readings[channel];
    xSemaphoreGive(s_lock);
}

void sensor_manager_detect(void)
{
    s_detect_requested = true;
}

const char *sensor_family_name(sensor_family_t family)
{
    static const char *names[] = {"None", "SHT4x", "SHT3x", "SHT2x", "Other"};
    if (family >= FAMILY_NONE && family <= FAMILY_OTHER) {
        return names[family];
    }
    return "None";
}
