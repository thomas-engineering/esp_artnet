#pragma once

/* Starts the background OTA task.
   Checks GitHub releases every OTA_CHECK_INTERVAL_S seconds.
   If a newer tag is found, downloads and flashes the firmware. */
void ota_start(void);
