#include "log_buffer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOG_BUF_SIZE  8192
#define LOG_LINE_MAX  160
#define LOG_BUF_MAGIC 0x454E5638u /* "ENV8" */

typedef struct {
    uint32_t magic;
    uint32_t head; /* next write position */
    uint32_t len;  /* bytes in use, up to LOG_BUF_SIZE */
    uint32_t boots;
    char data[LOG_BUF_SIZE];
} log_ring_t;

/* Not zeroed at boot: what the previous run wrote is still there. */
static RTC_NOINIT_ATTR log_ring_t s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_filter_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external-pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "other-watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    default: return "unknown";
    }
}

static void append(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        s_ring.data[s_ring.head] = s[i];
        s_ring.head = (s_ring.head + 1) % LOG_BUF_SIZE;
        if (s_ring.len < LOG_BUF_SIZE) {
            s_ring.len++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

/*
 * The Wi-Fi driver prints about 80 info lines per connect (rates, A-MPDU,
 * HE capabilities), which would fill the buffer in one boot. Of its info
 * lines keep only the channel, state changes, connect and security lines;
 * warnings, errors and every other tag are kept. The serial port still gets
 * everything.
 *
 * The driver writes a line in three calls (the "I (ms) wifi:" prefix, the
 * text, then a newline), so the prefix is held until the text shows whether
 * the line is kept.
 */
#define PREFIX_MAX 32
static char s_pending[PREFIX_MAX];
static size_t s_pending_len;
static bool s_dropping; /* inside a dropped line until its '\n' */
/* Task writing the held or dropped line; other tasks' lines pass straight through. */
static TaskHandle_t s_owner;
/* A truncated wifi text already ended its line; skip the driver's "\n". */
static bool s_skip_nl;

static bool is_wifi_info(const char *s)
{
    return strncmp(s, "I (", 3) == 0 && strstr(s, ") wifi:") != NULL;
}

static bool wifi_text_kept(const char *s)
{
    /* "new:<6,0>, old:<1,0>" names the channel of each connect attempt. */
    return strncmp(s, "state:", 6) == 0 || strncmp(s, "connected with", 14) == 0
           || strncmp(s, "security:", 9) == 0 || strncmp(s, "new:<", 5) == 0
           || strstr(s, "wifi:state:") || strstr(s, "wifi:connected with")
           || strstr(s, "wifi:security:") || strstr(s, "wifi:new:<");
}

static void store(const char *s, size_t len)
{
    const bool ends_line = len > 0 && s[len - 1] == '\n';
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if ((s_pending_len || s_dropping || s_skip_nl) && self != s_owner) {
        append(s, len); /* another task's line, between the pieces of a wifi line */
        return;
    }
    if (s_skip_nl) {
        s_skip_nl = false;
        if (len == 1 && s[0] == '\n') {
            return;
        }
    }
    if (s_pending_len) {
        /* Text following a held wifi prefix. */
        if (wifi_text_kept(s)) {
            append(s_pending, s_pending_len);
            append(s, len);
        } else {
            s_dropping = !ends_line;
        }
        s_skip_nl = ends_line; /* the text was cut and given its own '\n' */
        s_pending_len = 0;
        return;
    }
    if (s_dropping) {
        s_dropping = !ends_line;
        return;
    }
    if (is_wifi_info(s)) {
        s_owner = self;
        if (!ends_line && len < PREFIX_MAX) {
            memcpy(s_pending, s, len);
            s_pending_len = len;
        } else if (wifi_text_kept(s)) {
            append(s, len);
        } else {
            s_dropping = !ends_line;
        }
        return;
    }
    append(s, len);
}

static int log_hook(const char *fmt, va_list args)
{
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        if ((size_t)n >= sizeof(line)) {
            line[len - 1] = '\n'; /* truncated: keep one line per entry */
        }
        portENTER_CRITICAL(&s_filter_mux);
        store(line, len);
        portEXIT_CRITICAL(&s_filter_mux);
    }
    return s_prev ? s_prev(fmt, args) : vprintf(fmt, args);
}

void log_buffer_init(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    /*
     * After a power-on LP RAM holds noise. A brownout is kept: with
     * CONFIG_ESP_BROWNOUT_USE_INTR it is a software restart (brownout.c sets
     * the reset hint), LP RAM survives, and the header check covers the rest.
     */
    const bool valid = s_ring.magic == LOG_BUF_MAGIC && s_ring.head < LOG_BUF_SIZE
                       && s_ring.len <= LOG_BUF_SIZE && reason != ESP_RST_POWERON;
    if (!valid) {
        memset(&s_ring, 0, sizeof(s_ring));
        s_ring.magic = LOG_BUF_MAGIC;
    }
    s_ring.boots++;

    if (s_ring.len && s_ring.data[(s_ring.head + LOG_BUF_SIZE - 1) % LOG_BUF_SIZE] != '\n') {
        append("\n", 1); /* the previous run stopped mid-line */
    }
    char mark[80];
    int n = snprintf(mark, sizeof(mark), "=== boot %u, reset reason %s ===\n",
                     (unsigned)s_ring.boots, reset_reason_name(reason));
    if (n > 0) {
        append(mark, (size_t)n < sizeof(mark) ? (size_t)n : sizeof(mark) - 1);
    }
    s_prev = esp_log_set_vprintf(log_hook);
}

size_t log_buffer_copy(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_mux);
    size_t len = s_ring.len < cap ? s_ring.len : cap;
    size_t start = (s_ring.head + LOG_BUF_SIZE - s_ring.len) % LOG_BUF_SIZE;
    /* With cap < len, keep the newest bytes. */
    start = (start + (s_ring.len - len)) % LOG_BUF_SIZE;
    const size_t first = len < LOG_BUF_SIZE - start ? len : LOG_BUF_SIZE - start;
    memcpy(out, &s_ring.data[start], first);
    memcpy(out + first, s_ring.data, len - first);
    portEXIT_CRITICAL(&s_mux);
    return len;
}

size_t log_buffer_size(void)
{
    return LOG_BUF_SIZE;
}
