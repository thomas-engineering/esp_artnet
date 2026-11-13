#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Starts the background OTA task. It checks GitHub at boot and every 6 h,
   but only while automatic updates are enabled (off by default). */
void ota_start(void);

/* Automatic update checks can be switched off (persisted in NVS, default OFF).
   Manual updates from the web UI are not affected. */
bool ota_get_auto_enabled(void);
void ota_set_auto_enabled(bool enabled);

/* Only one firmware update may run at a time (automatic check, flash from
   URL, upload). ota_try_lock() returns false if another one is in progress;
   every successful lock must be released with ota_unlock(). */
bool ota_try_lock(void);
void ota_unlock(void);

/* Called periodically during ota_flash_url() with the current progress (0–100). */
typedef void (*ota_progress_cb_t)(int pct);

/* Flashes firmware from an HTTPS URL immediately.
   Blocks until complete; call from a task with ≥ 8 kB stack.
   cb may be NULL. Returns ESP_OK on success (caller should esp_restart()). */
esp_err_t ota_flash_url(const char *url, ota_progress_cb_t cb);
