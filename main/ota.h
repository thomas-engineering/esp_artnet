#pragma once
#include "esp_err.h"

/* Starts the background OTA task (checks GitHub every 6 h). */
void ota_start(void);

/* Flashes firmware from an HTTPS URL immediately.
   Blocks until complete; call from a task with ≥ 8 kB stack.
   Returns ESP_OK on success (caller should esp_restart()). */
esp_err_t ota_flash_url(const char *url);
