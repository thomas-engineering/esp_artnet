#pragma once
#include <stdint.h>

#define DMX_CHANNELS    512
#define DMX_TX_GPIO     43
#define DMX_RX_GPIO     44
#define DMX_DE_GPIO     1

void dmx_init(void);
void dmx_send(const uint8_t *data, uint16_t length);
