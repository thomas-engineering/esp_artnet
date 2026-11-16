#pragma once
/* E1.31 (sACN) input. Only one input protocol is active at a time: when the
   mode is sACN the node ignores ArtDmx (ArtPoll / ArtAddress / ArtIpProg keep
   working so the node stays discoverable and configurable). */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    INPUT_ARTNET = 0,   /* default */
    INPUT_SACN   = 1,
} input_mode_t;

/* Loads the settings from NVS and starts the receiver task (call after
   artnet_start(); the output buffer is shared with Art-Net). */
void         sacn_start(void);

input_mode_t sacn_get_mode(void);
void         sacn_set_mode(input_mode_t mode);               /* stored in NVS */
static inline bool sacn_active(void) { return sacn_get_mode() == INPUT_SACN; }

/* Effective sACN universe (1..63999). 0 stored = "automatic": the Art-Net
   port-address + 1 (Art-Net counts from 0, sACN from 1). */
uint16_t     sacn_get_universe(void);
void         sacn_set_universe(uint16_t universe);           /* 0 = automatic */

int          sacn_get_sources(void);                         /* active senders */
