#pragma once
#include <stdint.h>

#define ARTNET_PORT 6454

void     artnet_start(uint16_t universe);
void     artnet_set_universe(uint16_t universe);

/* Request an unsolicited ArtPollReply broadcast (sent if a controller asked
   for updates via ArtPoll TalkToMe, and always once after boot). */
void     artnet_announce(void);
void     artnet_copy_dmx(uint8_t *dst, uint16_t max_len);
uint32_t artnet_get_pkt_count(void);
