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
    uint32_t roams;             /* moves made by join-strongest-ap since boot */
    uint32_t last_reason;       /* wifi_err_reason_t of the last disconnect, 0: none */
    int32_t last_disconnect_s;  /* uptime of the last disconnect, -1: none */
} network_wifi_status_t;

/* One consistent snapshot of the station link and its counters. */
void network_wifi_status(network_wifi_status_t *out);

typedef struct {
    char bssid[18]; /* empty when not associated */
    int channel;
    int rssi;
} network_ap_t;

typedef struct {
    network_ap_t before; /* AP in use; RSSI from the scan when it was seen in it */
    network_ap_t best;   /* strongest AP with the SSID in the scan */
    int aps_found;
    bool moving;         /* network_roam_move() must follow */
} network_roam_t;

/*
 * Background-scans for the configured SSID while connected, without dropping
 * the link. Sets moving when the strongest AP is margin_db or more above the
 * current one; the caller then answers its client and calls
 * network_roam_move(), since the move drops the link and lwIP aborts every
 * TCP connection on the old address.
 * ESP_ERR_INVALID_STATE: setup AP or not connected; ESP_ERR_NOT_FINISHED:
 * already running.
 */
esp_err_t network_roam_scan(int margin_db, network_roam_t *out);

/*
 * Leaves the current AP and joins the one chosen by network_roam_scan().
 * Blocks until linked again, up to 45 s, then restarts the connection.
 */
esp_err_t network_roam_move(void);

/* Station associated (WIFI_EVENT_STA_CONNECTED seen, no drop since). */
bool network_connected(void);
