#pragma once
#include <stdint.h>

/*
 * Debug-only DMX read-back test (enabled with -DDMX_DEBUG_READBACK).
 *
 * The DMX TX pad (UART1 TX) is routed through the GPIO matrix into the RX
 * input of UART2 (250 kbaud 8N2), which decodes the frames and compares them
 * with what dmx_send() sent. A summary is logged every 2 s.
 */
#ifdef DMX_DEBUG_READBACK
void dmx_selftest_start(int tap_gpio);
void dmx_selftest_expect(const uint8_t *data, uint16_t len);
#else
static inline void dmx_selftest_start(int tap_gpio) { (void)tap_gpio; }
static inline void dmx_selftest_expect(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
}
#endif
