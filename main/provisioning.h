#pragma once
#include <stdint.h>

/* Blocks until the device is connected to WiFi.
   On first boot: starts SoftAP "ARTNET_XXXXXX" + web portal at 192.168.4.1.
   On subsequent boots: reconnects with stored credentials. */
void provisioning_start(void);

/* Erase stored WiFi credentials (call before restart to force re-provisioning) */
void provisioning_clear(void);

/* Return the Art-Net universe stored in NVS (default 0 if never set) */
uint16_t provisioning_get_universe(void);
