#include "dmx_debug.h"

#if defined(DMX_DEBUG_TEST_PATTERN) || defined(DMX_DEBUG_REPLAY) || \
    defined(DMX_DEBUG_READBACK)     || defined(DMX_DEBUG_SNIFF)

#include <string.h>
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "dmx_selftest.h"
#include "dmx_sniff.h"

void dmx_debug_init(int tx_gpio)
{
#ifdef DMX_DEBUG_READBACK
    dmx_selftest_start(tx_gpio);
#endif
#ifdef DMX_DEBUG_SNIFF
    dmx_sniff_start();               /* listen only, never transmit */
#endif
    (void)tx_gpio;
}

bool dmx_debug_tx_disabled(void)
{
#ifdef DMX_DEBUG_SNIFF
    return true; /* sniffer mode: the bus belongs to the other master */
#else
    return false;
#endif
}

void dmx_debug_frame_sent(const uint8_t *data, uint16_t len)
{
#ifdef DMX_DEBUG_READBACK
    dmx_selftest_expect(data, len);
#endif
    (void)data;
    (void)len;
}

void dmx_debug_override_frame(uint8_t *buf, uint16_t len)
{
#ifdef DMX_DEBUG_REPLAY
    /* Replay of the Arduino DMX_Master sketch as captured on the bus:
     * ch1-3 fade 0..31..0 in steps of 1 every 30 ms, all other slots 0,
     * 513 slots per frame, one frame every ~26 ms. */
    static int val, dir = 1;
    static int64_t t_step;
    int64_t now = esp_timer_get_time();
    if (now - t_step >= 30000) {
        t_step = now;
        val += dir;
        if (val > 30) dir = -1;
        if (val < 1)  dir = 1;
    }
    memset(buf, 0, len);
    buf[0] = buf[1] = buf[2] = (uint8_t)val;
#endif
#ifdef DMX_DEBUG_TEST_PATTERN
    memset(buf, 255, 8); /* channels 1-8 full, bypasses Art-Net */
#endif
    (void)buf;
    (void)len;
}

bool dmx_debug_frame_pause(void)
{
#ifdef DMX_DEBUG_REPLAY
    esp_rom_delay_us(3300); /* UNO frame period is ~26.1 ms (6 us gap per slot) */
    return true;
#else
    return false;
#endif
}

#endif
