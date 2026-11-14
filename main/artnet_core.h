#pragma once
/* Hardware-independent Art-Net logic (no ESP-IDF, no FreeRTOS, no sockets).
   Kept separate from artnet.c so that it can be unit-tested on the host
   (see test/). All times are microseconds from any monotonic clock. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTNET_CORE_CHANNELS   512
#define ARTNET_DMX_HEADER_LEN  18

/* A source that has been silent this long no longer takes part in merging */
#define MERGE_TIMEOUT_US     (10 * 1000000LL)
/* After this long without ArtDmx the sequence check starts afresh */
#define SEQ_RESET_US         (1 * 1000000LL)
/* No ArtDmx for this long = data loss: apply the failsafe setting */
#define FAILSAFE_TIMEOUT_US  (6 * 1000000LL)

/* Up to two controllers may send to the universe; their data is merged */
typedef struct {
    uint32_t ip;                                /* network byte order, 0 = unused */
    int64_t  last_us;                           /* last accepted packet           */
    uint8_t  seq;                               /* last sequence number (0 = none) */
    uint8_t  data[ARTNET_CORE_CHANNELS];        /* last frame of this source      */
} artnet_source_t;

typedef struct {
    artnet_source_t src[2];
    bool            merging;                    /* two sources are active         */
    bool            merge_ltp;                  /* false = HTP                    */
    bool            cancel_merge;               /* AcCancelMerge pending          */
} artnet_merge_t;

/* Processes one ArtDmx packet (raw bytes from the "Art-Net\0" id on, `len`
   received bytes). On acceptance the merged result is written to `out`
   (ARTNET_CORE_CHANNELS bytes, only the first data_len bytes change) and true
   is returned. Rejected: too short, ProtVer < 14, wrong universe, empty,
   out-of-order sequence number, or a third source. */
bool artnet_core_dmx(artnet_merge_t *m, const uint8_t *p, int len, uint32_t src_ip,
                     int64_t now_us, uint16_t universe, uint8_t *out);

/* Clears the stored frames of both sources (ArtAddress AcClearOp) */
void artnet_core_clear(artnet_merge_t *m);

/* True once, when the failsafe must be applied now: failsafe mode is not
   "hold" (0), data was received before and has been missing for
   FAILSAFE_TIMEOUT_US, and it has not been applied yet. */
bool artnet_core_failsafe_due(uint8_t mode, bool already_applied,
                              int64_t last_dmx_us, int64_t now_us);

/* ArtPoll: true if the node has to answer. `flags` = TalkToMe byte; in
   targeted mode (bit 5) only if `universe` is inside [bottom, top]. */
bool artnet_core_poll_matches(const uint8_t *p, int len, uint16_t universe);

/* ArtAddress: new port-address from the Net (p[12]), SwOut (p[100]) and
   SubSwitch (p[104]) bytes. A part is only applied when bit 7 is set. */
uint16_t artnet_core_address_port(const uint8_t *p, uint16_t current);
