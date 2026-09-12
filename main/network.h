#pragma once

#include "esp_err.h"

esp_err_t network_init(void);
int network_rssi(void);
const char *network_mode(void);
