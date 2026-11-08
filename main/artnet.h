#pragma once
#include <stdint.h>

#define ARTNET_PORT 6454

void artnet_start(uint16_t universe);
void artnet_copy_dmx(uint8_t *dst, uint16_t max_len);
