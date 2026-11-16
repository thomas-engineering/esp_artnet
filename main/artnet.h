#pragma once
#include <stdint.h>
#include <stdbool.h>

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

/* Remote programming over Art-Net (ArtAddress, ArtIpProg). On by default;
   when off both commands are ignored (ArtIpProg is still answered with the
   current settings). Stored in NVS. */
bool artnet_get_remote_prog(void);
void artnet_set_remote_prog(bool allow);

/* GoodOutput byte for ArtPollReply (data active, merging, LTP) */
uint8_t  artnet_good_output(void);

/* Output buffer access for other input protocols (sACN). artnet_output_lock()
   takes the buffer mutex and returns the buffer (DMX_CHANNELS bytes); call
   artnet_output_unlock() afterwards. `updated` = the buffer was changed: it
   restarts the failsafe timer and counts as a received packet. */
uint8_t *artnet_output_lock(void);
void     artnet_output_unlock(bool updated, bool merging);
bool     artnet_merge_ltp(void);        /* merge mode selected with ArtAddress */
