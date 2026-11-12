#pragma once
#include <stdint.h>

#define DMX_CHANNELS    512
#define DMX_TX_GPIO     41   /* UART1 TX -> MAX3430 DI (pin 4) */
#define DMX_RX_GPIO     42   /* UART1 RX <- MAX3430 RO (pin 1), reserved */
#define DMX_DE_GPIO     1

void dmx_init(void);
void dmx_send(const uint8_t *data, uint16_t length);
