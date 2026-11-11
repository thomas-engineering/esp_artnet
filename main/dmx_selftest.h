#pragma once
#include <stdint.h>

/*
 * Debug-only DMX read-back test (enabled with -DDMX_DEBUG_READBACK).
 *
 * The DMX TX pad (UART0 TX) is routed through the GPIO matrix into the RX
 * inputs of UART1 (250 kbaud 8N2, decodes frames) and UART2 (120 kbaud, only
 * detects breaks >= 92 us). A summary is logged every 2 s.
 *
 * While active, the log output is redirected to USB-Serial-JTAG so that log
 * text does not get mixed into the DMX stream on UART0.
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
