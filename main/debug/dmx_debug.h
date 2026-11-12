#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Single entry point for all optional DMX debug / test features.
 *
 * Everything here compiles to empty inline stubs unless one of the
 * -DDMX_DEBUG_* options is given, so the production code (main.c, dmx.c)
 * can call these hooks unconditionally without any #ifdef.
 *
 *   -DDMX_DEBUG_TEST_PATTERN=1  channels 1-8 forced to 255 (bypasses Art-Net)
 *   -DDMX_DEBUG_REPLAY=1        fixed fade, mimics the Arduino DMX_Master
 *   -DDMX_DEBUG_READBACK=1      UART2 decodes our own TX line (dmx_selftest.c)
 *   -DDMX_DEBUG_SNIFF=1         listen-only bus sniffer (dmx_sniff.c)
 */
#if defined(DMX_DEBUG_TEST_PATTERN) || defined(DMX_DEBUG_REPLAY) || \
    defined(DMX_DEBUG_READBACK)     || defined(DMX_DEBUG_SNIFF)

/** Start the enabled debug helpers (called from dmx_init()). */
void dmx_debug_init(int tx_gpio);

/** True when dmx_send() must not drive the bus (sniffer mode). */
bool dmx_debug_tx_disabled(void);

/** Called with every frame that is about to be transmitted. */
void dmx_debug_frame_sent(const uint8_t *data, uint16_t len);

/** Overwrite the frame buffer with a debug pattern (test pattern / replay). */
void dmx_debug_override_frame(uint8_t *buf, uint16_t len);

/**
 * Optional replacement for the normal inter-frame pause.
 * Returns true if it paused (caller then skips its own delay).
 */
bool dmx_debug_frame_pause(void);

#else

static inline void dmx_debug_init(int tx_gpio)                { (void)tx_gpio; }
static inline bool dmx_debug_tx_disabled(void)                { return false; }
static inline void dmx_debug_frame_sent(const uint8_t *d, uint16_t n)
{
    (void)d;
    (void)n;
}
static inline void dmx_debug_override_frame(uint8_t *b, uint16_t n)
{
    (void)b;
    (void)n;
}
static inline bool dmx_debug_frame_pause(void)                { return false; }

#endif
