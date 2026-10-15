#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Confirms a freshly updated image once the network is up, cancelling rollback. */
esp_err_t ota_init(void);

/* POST /api/v1/firmware: raw .bin body, X-OTA-Token header. Reboots on success. */
esp_err_t ota_upload_handler(httpd_req_t *req);

const char *ota_running_partition(void);
bool ota_pending_verify(void);
