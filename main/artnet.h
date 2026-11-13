#pragma once
#include <stdint.h>

#define ARTNET_PORT 6454

/* What the output does when Art-Net data stops (ArtAddress AcFail*, web UI) */
typedef enum {
    ARTNET_FAILSAFE_HOLD = 0,   /* keep the last look (default) */
    ARTNET_FAILSAFE_ZERO = 1,   /* all channels to 0            */
    ARTNET_FAILSAFE_FULL = 2,   /* all channels to 255          */
} artnet_failsafe_t;

void     artnet_start(uint16_t universe);
void     artnet_set_universe(uint16_t universe);
uint16_t artnet_get_universe(void);

/* Request an unsolicited ArtPollReply broadcast (sent if a controller asked
   for updates via ArtPoll TalkToMe, and always once after boot). */
void     artnet_announce(void);

/* Merged DMX output buffer. Safe to call before artnet_start() (returns zeros). */
void     artnet_copy_dmx(uint8_t *dst, uint16_t max_len);

uint32_t artnet_get_pkt_count(void);
uint32_t artnet_get_pps(void);          /* accepted ArtDmx packets per second */
uint32_t artnet_idle_ms(void);          /* ms since the last ArtDmx, UINT32_MAX = never */

artnet_failsafe_t artnet_get_failsafe(void);
void              artnet_set_failsafe(artnet_failsafe_t mode);   /* stored in NVS */

/* GoodOutput byte for ArtPollReply (data active, merging, LTP) */
uint8_t  artnet_good_output(void);
