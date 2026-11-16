#include "sacn_core.h"
#include <string.h>

static const uint8_t ACN_ID[12] = { 'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0 };

#define OPT_TERMINATED  0x40
#define OPT_PREVIEW     0x80

static bool source_active(const sacn_source_t *s, int64_t now)
{
    return s->used && now - s->last_us < SACN_TIMEOUT_US;
}

/* Flags (high nibble 0x7) + 12-bit PDU length at p[0..1] */
static bool pdu_ok(const uint8_t *p, int offset, int len)
{
    if ((p[offset] & 0xF0) != 0x70) return false;
    int plen = ((p[offset] & 0x0F) << 8) | p[offset + 1];
    return offset + plen <= len;                /* never trust it beyond the packet */
}

static bool be32_is(const uint8_t *p, uint32_t v)
{
    return p[0] == (uint8_t)(v >> 24) && p[1] == (uint8_t)(v >> 16) &&
           p[2] == (uint8_t)(v >> 8)  && p[3] == (uint8_t)v;
}

bool sacn_universe_valid(unsigned universe)
{
    return universe >= 1 && universe <= SACN_UNIVERSE_MAX;
}

uint32_t sacn_multicast_addr(uint16_t universe)
{
    return 0xEFFF0000u | universe;              /* 239.255.hi.lo */
}

int sacn_core_active(const sacn_merge_t *m, int64_t now)
{
    return source_active(&m->src[0], now) + source_active(&m->src[1], now);
}

/* The slot for a new source: a free or expired one, else the one with the lowest
   priority (oldest first) if the new source has a higher priority than it. */
static sacn_source_t *take_slot(sacn_merge_t *m, int64_t now, uint8_t priority)
{
    for (int i = 0; i < 2; i++)
        if (!source_active(&m->src[i], now)) return &m->src[i];

    sacn_source_t *low = &m->src[0];
    if (m->src[1].priority < low->priority ||
        (m->src[1].priority == low->priority && m->src[1].last_us < low->last_us))
        low = &m->src[1];
    return priority > low->priority ? low : NULL;
}

sacn_rx_t sacn_core_dmx(sacn_merge_t *m, const uint8_t *p, int len, int64_t now,
                        uint16_t universe, bool ltp, uint8_t *out)
{
    if (len < SACN_MIN_LEN) return SACN_RX_IGNORED;

    /* Root layer */
    if (p[0] != 0x00 || p[1] != 0x10 || p[2] != 0x00 || p[3] != 0x00) return SACN_RX_IGNORED;
    if (memcmp(p + 4, ACN_ID, sizeof(ACN_ID)) != 0) return SACN_RX_IGNORED;
    if (!pdu_ok(p, 16, len) || !be32_is(p + 18, 0x00000004)) return SACN_RX_IGNORED;

    /* Framing layer */
    if (!pdu_ok(p, 38, len) || !be32_is(p + 40, 0x00000002)) return SACN_RX_IGNORED;
    uint8_t  priority = p[108];
    uint8_t  seq      = p[111];
    uint8_t  options  = p[112];
    uint16_t univ     = (uint16_t)((p[113] << 8) | p[114]);
    if (univ != universe) return SACN_RX_IGNORED;
    if (priority > SACN_PRIORITY_MAX) return SACN_RX_IGNORED;
    if (options & OPT_PREVIEW) return SACN_RX_IGNORED;     /* for visualisers, not fixtures */

    /* DMP layer */
    if (!pdu_ok(p, 115, len)) return SACN_RX_IGNORED;
    if (p[117] != 0x02 || p[118] != 0xA1) return SACN_RX_IGNORED;
    if (p[119] != 0 || p[120] != 0 || p[121] != 0 || p[122] != 1) return SACN_RX_IGNORED;
    unsigned count = ((unsigned)p[123] << 8) | p[124];      /* start code + channels */
    if (count < 1 || count > SACN_CHANNELS + 1) return SACN_RX_IGNORED;
    if (SACN_OFF_START_CODE + (int)count > len) return SACN_RX_IGNORED;
    if (p[SACN_OFF_START_CODE] != 0x00) return SACN_RX_IGNORED;   /* only NULL start code */
    unsigned n = count - 1;
    const uint8_t *data = p + SACN_OFF_DATA;

    /* Find the source by its CID */
    sacn_source_t *slot = NULL;
    for (int i = 0; i < 2; i++)
        if (m->src[i].used && memcmp(m->src[i].cid, p + 22, 16) == 0) slot = &m->src[i];

    sacn_source_t *other = NULL;
    if (options & OPT_TERMINATED) {             /* Stream_Terminated: the source is gone */
        if (!slot) return SACN_RX_IGNORED;
        other = (slot == &m->src[0]) ? &m->src[1] : &m->src[0];
        memset(slot, 0, sizeof(*slot));
        m->merging = false;
        if (source_active(other, now)) {        /* the other source takes over */
            memcpy(out, other->data, other->len);
            return SACN_RX_OUTPUT;
        }
        return SACN_RX_TERMINATED;
    }
    if (n == 0) return SACN_RX_IGNORED;

    bool fresh = !slot || !source_active(slot, now);
    if (!slot) {
        slot = take_slot(m, now, priority);
        if (!slot) return SACN_RX_IGNORED;      /* a third source without priority */
    }
    if (fresh) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->cid, p + 22, 16);
    } else {
        /* Sequence check: drop duplicates and packets that arrive out of order */
        int8_t diff = (int8_t)(uint8_t)(seq - slot->seq);
        if (diff <= 0 && diff > -20) return SACN_RX_IGNORED;
    }
    other = (slot == &m->src[0]) ? &m->src[1] : &m->src[0];

    bool other_active = source_active(other, now);
    sacn_rx_t res = SACN_RX_OUTPUT;

    if (other_active && other->priority > priority) {
        m->merging = false;                     /* a higher-priority source owns the output */
        res = SACN_RX_STORED;
    } else if (!other_active || other->priority < priority) {
        m->merging = false;
        memcpy(out, data, n);
    } else {
        m->merging = true;
        if (ltp) {
            /* LTP: a channel follows whichever source changed it last */
            for (unsigned i = 0; i < n; i++)
                if (data[i] != slot->data[i]) out[i] = data[i];
        } else {
            /* HTP: the highest level of both sources wins */
            unsigned top = n > other->len ? n : other->len;
            for (unsigned i = 0; i < top; i++) {
                uint8_t a = i < n ? data[i] : 0;
                out[i] = a > other->data[i] ? a : other->data[i];
            }
        }
    }

    slot->seq      = seq;
    slot->priority = priority;
    slot->last_us  = now;
    slot->len      = (uint16_t)n;
    memcpy(slot->name, p + 44, 64);
    slot->name[64] = '\0';
    memcpy(slot->data, data, n);
    memset(slot->data + n, 0, SACN_CHANNELS - n);
    return res;
}
