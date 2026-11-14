#include "artnet_core.h"
#include <string.h>

static bool source_active(const artnet_source_t *s, int64_t now)
{
    return s->ip != 0 && now - s->last_us < MERGE_TIMEOUT_US;
}

bool artnet_core_dmx(artnet_merge_t *m, const uint8_t *p, int len, uint32_t src_ip,
                     int64_t now, uint16_t universe, uint8_t *out)
{
    if (len <= ARTNET_DMX_HEADER_LEN) return false;
    if ((((unsigned)p[10] << 8) | p[11]) < 14) return false;        /* ProtVer (BE) */
    if ((uint16_t)(p[14] | (p[15] << 8)) != universe) return false;  /* port-address (LE) */

    /* Never trust the length field beyond what was actually received */
    unsigned data_len = ((unsigned)p[16] << 8) | p[17];             /* BE */
    if (data_len > (unsigned)(len - ARTNET_DMX_HEADER_LEN)) data_len = len - ARTNET_DMX_HEADER_LEN;
    if (data_len > ARTNET_CORE_CHANNELS) data_len = ARTNET_CORE_CHANNELS;
    if (data_len == 0) return false;
    const uint8_t *data = p + ARTNET_DMX_HEADER_LEN;

    /* Find the slot of this source, or take a free / expired one */
    artnet_source_t *slot = NULL;
    for (int i = 0; i < 2; i++)
        if (m->src[i].ip == src_ip && source_active(&m->src[i], now)) slot = &m->src[i];
    if (!slot) {
        for (int i = 0; i < 2 && !slot; i++) {
            if (!source_active(&m->src[i], now)) {
                slot = &m->src[i];
                memset(slot, 0, sizeof(*slot));
                slot->ip = src_ip;
            }
        }
    }
    if (!slot) return false;                    /* a third source is ignored */
    artnet_source_t *other = (slot == &m->src[0]) ? &m->src[1] : &m->src[0];

    /* Sequence check: drop packets that arrive out of order. 0 disables it;
       after a pause the sender may have restarted, so start afresh. */
    uint8_t seq = p[12];
    if (seq != 0 && slot->seq != 0 && now - slot->last_us < SEQ_RESET_US &&
        (uint8_t)(seq - slot->seq) >= 0x80) {
        return false;
    }
    slot->seq = seq;

    if (m->cancel_merge) {                      /* AcCancelMerge: this source wins */
        m->cancel_merge = false;
        other->ip = 0;
    }
    bool merging = source_active(other, now);
    m->merging = merging;

    if (!merging) {
        memcpy(out, data, data_len);
    } else if (m->merge_ltp) {
        /* LTP: a channel follows whichever source changed it last */
        for (unsigned i = 0; i < data_len; i++)
            if (data[i] != slot->data[i]) out[i] = data[i];
    } else {
        /* HTP: the highest level of both sources wins */
        for (unsigned i = 0; i < data_len; i++)
            out[i] = data[i] > other->data[i] ? data[i] : other->data[i];
    }

    memcpy(slot->data, data, data_len);
    slot->last_us = now;
    return true;
}

void artnet_core_clear(artnet_merge_t *m)
{
    memset(m->src[0].data, 0, ARTNET_CORE_CHANNELS);
    memset(m->src[1].data, 0, ARTNET_CORE_CHANNELS);
}

bool artnet_core_failsafe_due(uint8_t mode, bool already_applied,
                              int64_t last_dmx_us, int64_t now_us)
{
    if (mode == 0 || already_applied) return false;
    if (last_dmx_us == 0 || now_us - last_dmx_us < FAILSAFE_TIMEOUT_US) return false;
    return true;
}

bool artnet_core_poll_matches(const uint8_t *p, int len, uint16_t universe)
{
    uint8_t flags = len >= 13 ? p[12] : 0;
    if ((flags & 0x20) && len >= 18) {
        uint16_t top    = ((uint16_t)p[14] << 8) | p[15];
        uint16_t bottom = ((uint16_t)p[16] << 8) | p[17];
        if (universe < bottom || universe > top) return false;
    }
    return true;
}

uint16_t artnet_core_address_port(const uint8_t *p, uint16_t u)
{
    if (p[12]  & 0x80) u = (u & 0x00FF) | ((uint16_t)(p[12]  & 0x7F) << 8);   /* net      */
    if (p[104] & 0x80) u = (u & 0x7F0F) | ((uint16_t)(p[104] & 0x0F) << 4);   /* sub-net  */
    if (p[100] & 0x80) u = (u & 0x7FF0) |  (uint16_t)(p[100] & 0x0F);         /* universe */
    return u;
}
