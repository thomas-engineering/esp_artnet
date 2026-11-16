#pragma once
#include <stdint.h>
#include "hal/uart_types.h"

/* ── DMX512 output interface (single place for all hardware definitions) ──
   MAX3430 RS-485 transceiver on UART1: A -> XLR 3 (Data+), B -> XLR 2 (Data-). */

#define DMX_CHANNELS    512

/* Pins */
#define DMX_TX_GPIO     41   /* UART1 TX -> MAX3430 DI (pin 4) */
#define DMX_RX_GPIO     42   /* UART1 RX <- MAX3430 RO (pin 1), reserved */
#define DMX_DE_GPIO     1    /* MAX3430 DE + RE (pins 2+3): high = transmit */

/* UART */
#define DMX_UART_NUM    UART_NUM_1   /* UART0 stays the console */
#define DMX_BAUD        250000       /* 8N2 */

/* Frame timing (ANSI E1.11 DMX512-A: BREAK >= 92 us, MARK AFTER BREAK >= 12 us,
   slot = 44 us). 180 us / 40 us is what a 0x00 byte at 49950 baud 8N2 produces:
   well inside the limits and accepted by receivers that expect the older, more
   lenient timing of the original DMX512 (1986). A full frame of 513 slots then
   takes 180 + 40 + 513 * 44 = 22.8 ms (about 43.9 frames per second). */
#define DMX_BREAK_US    180
#define DMX_MAB_US      40

void dmx_init(void);
void dmx_send(const uint8_t *data, uint16_t length);
