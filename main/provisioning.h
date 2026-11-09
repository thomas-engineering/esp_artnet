#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Blocks until the device is connected to WiFi.
   On first boot: starts SoftAP "ARTNET_XXXXXX" + web portal at 192.168.4.1.
   On subsequent boots: reconnects with stored credentials. */
void provisioning_start(void);

/* Erase stored WiFi credentials (call before restart to force re-provisioning) */
void provisioning_clear(void);

/* Art-Net universe (NVS-backed) */
uint16_t provisioning_get_universe(void);
void     provisioning_save_universe(uint16_t univ);

/* Network configuration (NVS-backed) */
typedef struct {
    bool dhcp;        /* true = DHCP, false = static */
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
} net_config_t;

void provisioning_get_net_config(net_config_t *cfg);
void provisioning_save_net_config(const net_config_t *cfg);
