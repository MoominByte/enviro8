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
static SemaphoreHandle_t s_evt_lock;
static volatile bool s_detect_requested = true;
static bool s_mux_present;
static sensor_debug_event_t s_events[SENSOR_DEBUG_EVENT_MAX];
static int s_evt_head;
static int s_evt_count;

static int s_i2c_ch = -1;
static uint8_t s_i2c_addr;
static const char *s_i2c_op = "-";
static bool s_i2c_quiet;
static uint8_t s_ch_addr[APP_CHANNELS];

static void i2c_ctx(int channel, uint8_t addr, const char *op)
{
    s_i2c_ch = channel;
    s_i2c_addr = addr;
    s_i2c_op = op ? op : "-";
}

static void debug_push(int channel, uint8_t addr, const char *dir, const char *op, const char *error)
{
    if (!s_evt_lock) {
        return;
    }
    sensor_debug_event_t ev = {
        .time_ms = (uint64_t)(esp_timer_get_time() / 1000),
        .channel = channel + 1,
        .addr = addr,
    };
    strncpy(ev.dir, dir ? dir : "-", sizeof(ev.dir) - 1);
    strncpy(ev.op, op ? op : "-", sizeof(ev.op) - 1);
    strncpy(ev.error, error ? error : "-", sizeof(ev.error) - 1);

    xSemaphoreTake(s_evt_lock, portMAX_DELAY);
    s_events[s_evt_head] = ev;
    s_evt_head = (s_evt_head + 1) % SENSOR_DEBUG_EVENT_MAX;
    if (s_evt_count < SENSOR_DEBUG_EVENT_MAX) {
        s_evt_count++;
    }
    xSemaphoreGive(s_evt_lock);
}

static void log_i2c_fail(const char *dir, esp_err_t err)
{
    if (s_i2c_quiet) {
        return;
    }
    debug_push(s_i2c_ch, s_i2c_addr, dir, s_i2c_op, esp_err_to_name(err));
    ESP_LOGW(TAG, "CH%d I2C %s failed during %s (addr 0x%02X): %s",
             s_i2c_ch + 1, dir, s_i2c_op, s_i2c_addr, esp_err_to_name(err));
}

static void bus_recover(void);

static void mux_reselect(void)
{
    if (s_i2c_ch < 0 || s_i2c_ch >= APP_CHANNELS) {
        return;
    }
    uint8_t mask = (uint8_t)(1u << s_i2c_ch);
    i2c_master_transmit(s_mux, &mask, 1, I2C_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(2));
}

static esp_err_t i2c_tx(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len)
{
    esp_err_t err = i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
    if (err == ESP_ERR_INVALID_STATE) {
        bus_recover();
        mux_reselect();
        err = i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        log_i2c_fail("write", err);
    }
    return err;
}

static esp_err_t i2c_rx(i2c_master_dev_handle_t dev, uint8_t *data, size_t len)
{
    esp_err_t err = i2c_master_receive(dev, data, len, I2C_TIMEOUT_MS);
    if (err == ESP_ERR_INVALID_STATE) {
        bus_recover();
        mux_reselect();
        err = i2c_master_receive(dev, data, len, I2C_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        log_i2c_fail("read", err);
    }
    return err;
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
    i2c_master_transmit(s_mux, &off, 1, I2C_TIMEOUT_MS);
}

static esp_err_t mux_select(int channel)
{
    i2c_ctx(channel, TCA_ADDR, "mux select");
    uint8_t mask = (uint8_t)(1u << channel);
    esp_err_t err = i2c_tx(s_mux, &mask, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH%d mux select failed (%s), resetting bus", channel + 1, esp_err_to_name(err));
        bus_recover();
        i2c_ctx(channel, TCA_ADDR, "mux select retry");
        err = i2c_tx(s_mux, &mask, 1);
    }
    /* Analog switch needs a moment before the downstream device is addressed. */
    vTaskDelay(pdMS_TO_TICKS(2));
    return err;
}

static void mux_disable(void)
{
    uint8_t off = 0;
    i2c_master_transmit(s_mux, &off, 1, I2C_TIMEOUT_MS);
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
    s_i2c_quiet = true;
    i2c_tx(dev, brk, sizeof(brk));
    vTaskDelay(pdMS_TO_TICKS(1));
    i2c_tx(dev, rst, sizeof(rst));
    s_i2c_quiet = false;
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void sht4x_recover(i2c_master_dev_handle_t dev)
{
    const uint8_t rst = 0x94;
    s_i2c_quiet = true;
    i2c_tx(dev, &rst, 1);
    s_i2c_quiet = false;
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void sht4x_read_serial(i2c_master_dev_handle_t dev, char *serial)
{
    uint8_t cmd = 0x89;
    uint8_t raw[6];
    i2c_ctx(s_i2c_ch, s_i2c_addr, "SHT4x serial");
    if (i2c_tx(dev, &cmd, 1) != ESP_OK) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3));
    if (i2c_rx(dev, raw, sizeof(raw)) == ESP_OK && crc_ok(raw)) {
        snprintf(serial, 24, "%02X%02X%02X%02X", raw[0], raw[1], raw[3], raw[4]);
    }
}

/*
 * SHT4x uses 8-bit commands. 0xFD = high-precision T+RH (max 8.3 ms).
 * Do not send SHT3x 0x2400 here: 0x24 is the SHT4x 110 mW / 0.1 s heater.
 * Serial (0x89) is optional and must never abort a measurement.
 */
static esp_err_t read_sht4x(i2c_master_dev_handle_t dev, float *temp, float *humidity,
                            char *serial, bool read_serial)
{
    uint8_t raw[6];

    if (read_serial && serial) {
        sht4x_read_serial(dev, serial);
    }

    uint8_t cmd = 0xFD;
    i2c_ctx(s_i2c_ch, s_i2c_addr, "SHT4x measure");
    esp_err_t err = i2c_tx(dev, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(15));
    err = i2c_rx(dev, raw, sizeof(raw));
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));
        i2c_ctx(s_i2c_ch, s_i2c_addr, "SHT4x measure retry");
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err != ESP_OK || !crc_ok(raw)) {
        if (err == ESP_OK) {
            debug_push(s_i2c_ch, s_i2c_addr, "crc", "SHT4x measure", "ESP_ERR_INVALID_CRC");
            ESP_LOGW(TAG, "CH%d SHT4x CRC mismatch (addr 0x%02X)", s_i2c_ch + 1, s_i2c_addr);
        }
        return err ? err : ESP_ERR_INVALID_CRC;
    }

    uint16_t t_raw = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t h_raw = ((uint16_t)raw[3] << 8) | raw[4];
    *temp = -45.0f + 175.0f * (t_raw / 65535.0f);
    *humidity = -6.0f + 125.0f * (h_raw / 65535.0f);
    if (*humidity < 0.0f) {
        *humidity = 0.0f;
    } else if (*humidity > 100.0f) {
        *humidity = 100.0f;
    }
    return ESP_OK;
}

static esp_err_t read_sht3x(i2c_master_dev_handle_t dev, float *temp, float *humidity)
{
    const uint8_t cmd[] = {0x24, 0x00};
    uint8_t raw[6];

    i2c_ctx(s_i2c_ch, s_i2c_addr, "SHT3x measure");
    esp_err_t err = i2c_tx(dev, cmd, sizeof(cmd));
    if (err == ESP_OK) {
        /* SHT35 high-repeatability conversion is 12.5 ms typ / 15.5 ms max. */
        vTaskDelay(pdMS_TO_TICKS(20));
        err = i2c_rx(dev, raw, sizeof(raw));
    }
    if (err != ESP_OK || !crc_ok(raw)) {
        if (err == ESP_OK) {
            debug_push(s_i2c_ch, s_i2c_addr, "crc", "SHT3x measure", "ESP_ERR_INVALID_CRC");
            ESP_LOGW(TAG, "CH%d SHT3x CRC mismatch (addr 0x%02X)", s_i2c_ch + 1, s_i2c_addr);
        }
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

static uint8_t preferred_sht_addr(int channel)
{
    return s_ch_addr[channel] ? s_ch_addr[channel] : SHT_ADDR_A;
}

static void remember_addr(int channel, uint8_t addr)
{
    s_ch_addr[channel] = addr;
}

static esp_err_t read_sht45_at(int channel, sensor_family_t family, bool detect,
                               float *temp, float *humidity, char *serial, uint8_t *used_addr)
{
    uint8_t first = preferred_sht_addr(channel);
    uint8_t addrs[2] = { first, (uint8_t)(first == SHT_ADDR_A ? SHT_ADDR_B : SHT_ADDR_A) };

    for (int i = 0; i < 2; i++) {
        uint8_t addr = addrs[i];
        i2c_master_dev_handle_t dev = handle_for_addr(addr);
        i2c_ctx(channel, addr, "sample");
        esp_err_t err = ESP_ERR_NOT_FOUND;
        if (family == FAMILY_SHT4X) {
            if (detect) {
                sht4x_recover(dev);
            }
            err = read_sht4x(dev, temp, humidity, serial, detect);
        } else {
            if (detect) {
                sht3x_recover(dev);
            }
            err = read_sht3x(dev, temp, humidity);
        }
        if (err == ESP_OK) {
            *used_addr = addr;
            remember_addr(channel, addr);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
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

    sensor_family_t family = family_from_type(type);
    uint8_t addr = preferred_sht_addr(channel);
    i2c_master_dev_handle_t dev = handle_for_addr(addr);
    i2c_ctx(channel, addr, "sample");
    float temp = NAN;
    float humidity = NAN;
    char serial[24] = {0};

    if (family == FAMILY_SHT4X || family == FAMILY_SHT3X) {
        err = read_sht45_at(channel, family, detect, &temp, &humidity, serial, &addr);
    } else if (family == FAMILY_SHT2X) {
        i2c_ctx(channel, SHT2X_ADDR, "sample");
        err = read_sht2x(s_dev_40, &temp, &humidity);
    } else if (type == SENSOR_AUTO) {
        /*
         * Identify with SHT4x 0xFD first. Never send SHT3x 0x2400 to an
         * unknown 0x44/0x45 device: 0x24 starts the SHT45 heater.
         * Do not use i2c_master_probe: a NACK leaves the IDF 5.5 bus in
         * ESP_ERR_INVALID_STATE and breaks every following channel.
         */
        if (read_sht45_at(channel, FAMILY_SHT4X, detect, &temp, &humidity, serial, &addr) == ESP_OK) {
            family = FAMILY_SHT4X;
            err = ESP_OK;
        } else if (read_sht45_at(channel, FAMILY_SHT3X, detect, &temp, &humidity, serial, &addr) == ESP_OK) {
            family = FAMILY_SHT3X;
            err = ESP_OK;
        } else {
            i2c_ctx(channel, SHT2X_ADDR, "sample");
            if (read_sht2x(s_dev_40, &temp, &humidity) == ESP_OK) {
                family = FAMILY_SHT2X;
                err = ESP_OK;
            } else {
                err = ESP_ERR_NOT_FOUND;
            }
        }
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
                reading.last_error[0] = '\0';
                if (detect) {
                    ESP_LOGI(TAG, "CH%d detected %s T=%.2f H=%.2f",
                             i + 1, sensor_family_name(reading.family),
                             reading.raw_temperature, reading.raw_humidity);
                }
            } else if (err != ESP_ERR_NOT_SUPPORTED) {
                reading.error_count++;
                strncpy(reading.last_error, esp_err_to_name(err), sizeof(reading.last_error) - 1);
                reading.last_error[sizeof(reading.last_error) - 1] = '\0';
                ESP_LOGW(TAG, "CH%d %s failed (%s): %s",
                         i + 1, detect ? "detect" : "sample",
                         sensor_type_name(reading.configured), esp_err_to_name(err));
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
    s_evt_lock = xSemaphoreCreateMutex();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    /* Driver NACKs have no channel context; we log CH1–CH8 ourselves. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "bus");
    ESP_RETURN_ON_ERROR(add_dev(TCA_ADDR, &s_mux), TAG, "mux");
    ESP_RETURN_ON_ERROR(add_dev(SHT_ADDR_A, &s_dev_44), TAG, "0x44");
    ESP_RETURN_ON_ERROR(add_dev(SHT_ADDR_B, &s_dev_45), TAG, "0x45");
    ESP_RETURN_ON_ERROR(add_dev(SHT2X_ADDR, &s_dev_40), TAG, "0x40");

    uint8_t mux_off = 0;
    s_mux_present = i2c_master_transmit(s_mux, &mux_off, 1, I2C_TIMEOUT_MS) == ESP_OK;
    if (!s_mux_present) {
        ESP_LOGW(TAG, "TCA9548A not found at 0x%02X — check SDA/SCL, A0-A2 and RESET", TCA_ADDR);
        debug_push(-1, TCA_ADDR, "probe", "mux", "not found");
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

void sensor_manager_i2c_info(sensor_i2c_info_t *out)
{
    out->freq_hz = I2C_FREQ_HZ;
    out->sda = I2C_SDA;
    out->scl = I2C_SCL;
    out->mux_addr = TCA_ADDR;
    out->mux_present = s_mux_present;
}

int sensor_manager_debug_copy(sensor_debug_event_t *out, int max)
{
    if (max <= 0) {
        return 0;
    }
    xSemaphoreTake(s_evt_lock, portMAX_DELAY);
    int n = s_evt_count < max ? s_evt_count : max;
    int start = (s_evt_head - s_evt_count + SENSOR_DEBUG_EVENT_MAX) % SENSOR_DEBUG_EVENT_MAX;
    for (int i = 0; i < n; i++) {
        out[n - 1 - i] = s_events[(start + i) % SENSOR_DEBUG_EVENT_MAX];
    }
    xSemaphoreGive(s_evt_lock);
    return n;
}

void sensor_manager_debug_clear(void)
{
    xSemaphoreTake(s_evt_lock, portMAX_DELAY);
    s_evt_head = 0;
    s_evt_count = 0;
    xSemaphoreGive(s_evt_lock);
}

const char *sensor_family_name(sensor_family_t family)
{
    static const char *names[] = {"None", "SHT4x", "SHT3x", "SHT2x", "Other"};
    if (family >= FAMILY_NONE && family <= FAMILY_OTHER) {
        return names[family];
    }
    return "None";
}
