#pragma once

#include <stddef.h>

#include "esp_system.h"

/*
 * Keeps the last ESP_LOG lines in LP RAM, which survives a software reset,
 * a panic, the watchdogs and a brownout (not a power loss), so the lines
 * before a crash can be read after the reboot. Wi-Fi driver info lines are
 * filtered (see log_buffer.c). Lines still go to the serial port.
 * Call first in app_main.
 */
void log_buffer_init(void);

/* Copies the buffer, oldest line first. Returns the number of bytes. */
size_t log_buffer_copy(char *out, size_t cap);

/* Size of the buffer in bytes. */
size_t log_buffer_size(void);

/* "power-on", "software", "panic", "task-watchdog", ... */
const char *reset_reason_name(esp_reset_reason_t r);
