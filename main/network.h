#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t network_init(void);
const char *network_mode(void);

typedef struct {
    int rssi;                   /* dBm, 0 when not associated */
    int channel;                /* 0 when not associated */
    char bssid[18];             /* "AA:BB:CC:DD:EE:FF", empty when not associated */
    uint32_t disconnects;       /* losses of an established link since boot */
    uint32_t connect_failures;  /* failed connect attempts since boot */
    uint32_t last_reason;       /* wifi_err_reason_t of the last disconnect, 0: none */
    int32_t last_disconnect_s;  /* uptime of the last disconnect, -1: none */
} network_wifi_status_t;

/* One consistent snapshot of the station link and its counters. */
void network_wifi_status(network_wifi_status_t *out);
