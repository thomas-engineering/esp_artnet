#pragma once
/* Hardware-independent E1.31 (sACN) logic: packet validation, per-source
   sequence check, priority arbitration and merging of up to two sources.
   No ESP-IDF, no FreeRTOS, no sockets, so it can be unit-tested on the host
   (see test/). All times are microseconds from any monotonic clock. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SACN_PORT            5568
#define SACN_CHANNELS        512
#define SACN_UNIVERSE_MAX    63999

/* Packet layout (offsets in bytes). The slot data starts with the DMX start code. */
#define SACN_OFF_START_CODE  125
#define SACN_OFF_DATA        126
#define SACN_MIN_LEN         126        /* header + start code, no channel data   */
#define SACN_MAX_LEN         (SACN_OFF_DATA + SACN_CHANNELS)

/* E1.31 network data loss: a source silent for this long is dropped */
#define SACN_TIMEOUT_US      2500000LL

#define SACN_PRIORITY_MAX    200
#define SACN_PRIORITY_DEFAULT 100

typedef struct {
    bool     used;
    int64_t  last_us;                       /* last accepted packet               */
    uint8_t  cid[16];                       /* sender component identifier        */
    uint8_t  seq;                           /* last sequence number               */
    uint8_t  priority;                      /* 0-200                              */
    uint16_t len;                           /* channels in the last frame         */
    char     name[65];                      /* source name (NUL-terminated)       */
    uint8_t  data[SACN_CHANNELS];           /* last frame, zero-filled behind len */
} sacn_source_t;

typedef struct {
    sacn_source_t src[2];
    bool          merging;                  /* two equal-priority sources active  */
} sacn_merge_t;

typedef enum {
    SACN_RX_IGNORED = 0,    /* not a valid / wanted packet, nothing changed       */
    SACN_RX_STORED,         /* valid, but a higher-priority source owns the output */
    SACN_RX_OUTPUT,         /* `out` was updated                                  */
    SACN_RX_TERMINATED,     /* source ended its stream, `out` was not changed     */
} sacn_rx_t;

/* Processes one UDP payload received on port 5568. `out` (SACN_CHANNELS bytes)
   holds the current output; only the channels covered by the winning source(s)
   are changed.
   Rejected: too short, malformed root/framing/DMP layer, wrong vector, a
   universe other than `universe`, start code other than 0, priority above
   200, preview data, out-of-order sequence number (E1.31 4.1: a packet whose
   sequence number is 0..-19 behind the last one), or a third source that does
   not have a higher priority than both existing ones. Sources are identified
   by their CID. At equal priority the data is merged HTP, or LTP when
   `ltp` is set; the highest priority wins otherwise. */
sacn_rx_t sacn_core_dmx(sacn_merge_t *m, const uint8_t *p, int len, int64_t now_us,
                        uint16_t universe, bool ltp, uint8_t *out);

/* Number of sources that sent data within SACN_TIMEOUT_US */
int sacn_core_active(const sacn_merge_t *m, int64_t now_us);

/* Multicast group of a universe, host byte order: 239.255.<hi>.<lo> */
uint32_t sacn_multicast_addr(uint16_t universe);

/* Valid sACN universe numbers are 1..63999 */
bool sacn_universe_valid(unsigned universe);
