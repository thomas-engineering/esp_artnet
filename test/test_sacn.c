#include "test.h"
#include "sacn_core.h"
#include <string.h>

#define SEC 1000000LL

/* Builds an E1.31 data packet; returns its length. */
static int sacn_pkt(uint8_t *p, uint8_t cid_id, uint8_t seq, uint16_t universe,
                    uint8_t priority, int channels, uint8_t fill)
{
    int len = 126 + channels;
    memset(p, 0, SACN_MAX_LEN);
    p[1] = 0x10;                                         /* preamble size */
    memcpy(p + 4, "ASC-E1.17\0\0\0", 12);
    p[16] = 0x70 | ((len - 16) >> 8); p[17] = (len - 16) & 0xFF;
    p[21] = 0x04;                                        /* root vector: data */
    memset(p + 22, cid_id, 16);                          /* CID */
    p[38] = 0x70 | ((len - 38) >> 8); p[39] = (len - 38) & 0xFF;
    p[43] = 0x02;                                        /* framing vector */
    memcpy(p + 44, "test source", 11);
    p[108] = priority;
    p[111] = seq;
    p[113] = universe >> 8; p[114] = universe & 0xFF;
    p[115] = 0x70 | ((len - 115) >> 8); p[116] = (len - 115) & 0xFF;
    p[117] = 0x02; p[118] = 0xA1;
    p[122] = 1;                                          /* address increment */
    p[123] = (channels + 1) >> 8; p[124] = (channels + 1) & 0xFF;
    p[125] = 0;                                          /* start code */
    memset(p + 126, fill, channels);
    return len;
}

static sacn_merge_t M;
static uint8_t out[512], pkt[SACN_MAX_LEN];

static void reset(void) { memset(&M, 0, sizeof(M)); memset(out, 0, sizeof(out)); }

static sacn_rx_t rx(uint8_t cid, uint8_t seq, uint16_t uni, uint8_t prio, int ch,
                    uint8_t fill, int64_t now, bool ltp)
{
    int len = sacn_pkt(pkt, cid, seq, uni, prio, ch, fill);
    return sacn_core_dmx(&M, pkt, len, now, 1, ltp, out);
}

static void test_sacn_basic(void)
{
    reset();
    CHECK_EQ(rx(1, 0, 1, 100, 512, 77, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 77); CHECK_EQ(out[511], 77);
    CHECK(!M.merging);
    CHECK_EQ(sacn_core_active(&M, SEC), 1);
    CHECK(strcmp(M.src[0].name, "test source") == 0);

    reset();                                            /* shorter frame: rest unchanged */
    memset(out, 9, sizeof(out));
    CHECK_EQ(rx(1, 0, 1, 100, 10, 5, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[9], 5); CHECK_EQ(out[10], 9);

    reset();
    CHECK_EQ(rx(1, 0, 2, 100, 512, 77, SEC, false), SACN_RX_IGNORED);   /* wrong universe */
    CHECK_EQ(out[0], 0);
    CHECK_EQ(rx(1, 0, 1, 201, 512, 77, SEC, false), SACN_RX_IGNORED);   /* priority > 200 */
    CHECK_EQ(rx(1, 0, 1, 100, 0, 77, SEC, false), SACN_RX_IGNORED);     /* no channels */
}

static void test_sacn_malformed(void)
{
    reset();
    int len = sacn_pkt(pkt, 1, 0, 1, 100, 512, 9);
    CHECK(sacn_core_dmx(&M, pkt, len, SEC, 1, false, out) == SACN_RX_OUTPUT);

    uint8_t bad[SACN_MAX_LEN];
    #define BREAK(expr) do { reset(); memcpy(bad, pkt, len); expr; \
        CHECK_EQ(sacn_core_dmx(&M, bad, len, SEC, 1, false, out), SACN_RX_IGNORED); } while (0)
    BREAK(bad[1] = 0x11);                               /* preamble */
    BREAK(bad[3] = 1);                                  /* postamble */
    BREAK(bad[4] = 'X');                                /* ACN id */
    BREAK(bad[21] = 0x08);                              /* root vector (extended) */
    BREAK(bad[16] = 0x10);                              /* root flags */
    BREAK(bad[43] = 0x01);                              /* framing vector (sync) */
    BREAK(bad[117] = 0x01);                             /* DMP vector */
    BREAK(bad[118] = 0x00);                             /* address type */
    BREAK(bad[122] = 2);                                /* increment */
    BREAK(bad[125] = 0xCC);                             /* RDM start code */
    BREAK(bad[112] = 0x80);                             /* preview data */
    BREAK(bad[123] = 0x03; bad[124] = 0x00);            /* property count > 513 */
    BREAK(bad[17] = 0xFF);                              /* root length beyond packet */
    #undef BREAK

    reset();                                            /* declared more channels than sent */
    CHECK_EQ(sacn_core_dmx(&M, pkt, len - 100, SEC, 1, false, out), SACN_RX_IGNORED);
    CHECK_EQ(sacn_core_dmx(&M, pkt, 125, SEC, 1, false, out), SACN_RX_IGNORED);
    CHECK_EQ(sacn_core_dmx(&M, pkt, 10, SEC, 1, false, out), SACN_RX_IGNORED);
}

static void test_sacn_sequence(void)
{
    reset();
    CHECK_EQ(rx(1, 10, 1, 100, 4, 1, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(1, 10, 1, 100, 4, 2, SEC + 1, false), SACN_RX_IGNORED);   /* duplicate */
    CHECK_EQ(rx(1, 9, 1, 100, 4, 2, SEC + 2, false), SACN_RX_IGNORED);    /* behind */
    CHECK_EQ(rx(1, 247, 1, 100, 4, 2, SEC + 3, false), SACN_RX_IGNORED);  /* -19 */
    CHECK_EQ(rx(1, 11, 1, 100, 4, 3, SEC + 4, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 3);
    CHECK_EQ(rx(1, 200, 1, 100, 4, 4, SEC + 5, false), SACN_RX_OUTPUT);   /* -67: restart */

    reset();                                            /* wrap-around 255 -> 0 */
    CHECK_EQ(rx(1, 255, 1, 100, 4, 1, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(1, 0, 1, 100, 4, 2, SEC + 1, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 2);

    reset();                                            /* after a pause the sender restarts */
    CHECK_EQ(rx(1, 50, 1, 100, 4, 1, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(1, 49, 1, 100, 4, 2, SEC + 3 * SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 2);
}

static void test_sacn_merge(void)
{
    reset();                                            /* HTP at equal priority */
    CHECK_EQ(rx(1, 0, 1, 100, 4, 50, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 100, 4, 30, SEC + 1, false), SACN_RX_OUTPUT);
    CHECK(M.merging);
    CHECK_EQ(out[0], 50);
    CHECK_EQ(sacn_core_active(&M, SEC + 1), 2);
    CHECK_EQ(rx(1, 1, 1, 100, 4, 10, SEC + 2, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 30);

    reset();                                            /* LTP: latest change wins */
    CHECK_EQ(rx(1, 0, 1, 100, 4, 50, SEC, true), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 100, 4, 30, SEC + 1, true), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 30);
    CHECK_EQ(rx(1, 1, 1, 100, 4, 60, SEC + 2, true), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 60);

    reset();                                            /* HTP with different lengths */
    CHECK_EQ(rx(1, 0, 1, 100, 8, 5, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 100, 4, 9, SEC + 1, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[3], 9); CHECK_EQ(out[7], 5);

    reset();                                            /* a source times out */
    CHECK_EQ(rx(1, 0, 1, 100, 4, 50, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 100, 4, 30, SEC + 1, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 1, 1, 100, 4, 20, SEC + 3 * SEC, false), SACN_RX_OUTPUT);
    CHECK(!M.merging);
    CHECK_EQ(out[0], 20);
}

static void test_sacn_priority(void)
{
    reset();
    CHECK_EQ(rx(1, 0, 1, 100, 4, 10, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 150, 4, 200, SEC + 1, false), SACN_RX_OUTPUT);   /* higher wins */
    CHECK_EQ(out[0], 200); CHECK(!M.merging);
    CHECK_EQ(rx(1, 1, 1, 100, 4, 255, SEC + 2, false), SACN_RX_STORED);   /* lower: no output */
    CHECK_EQ(out[0], 200);
    /* the high-priority source stops: the other one is used again */
    CHECK_EQ(rx(1, 2, 1, 100, 4, 33, SEC + 4 * SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 33);

    reset();                                            /* third source */
    CHECK_EQ(rx(1, 0, 1, 100, 4, 1, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 100, 4, 2, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(3, 0, 1, 100, 4, 3, SEC, false), SACN_RX_IGNORED);        /* same priority */
    CHECK_EQ(rx(3, 0, 1, 50, 4, 3, SEC, false), SACN_RX_IGNORED);         /* lower */
    CHECK_EQ(rx(3, 0, 1, 120, 4, 3, SEC + 1, false), SACN_RX_OUTPUT);     /* evicts one */
    CHECK_EQ(out[0], 3);
    CHECK_EQ(sacn_core_active(&M, SEC + 1), 2);
}

static void test_sacn_terminate(void)
{
    reset();
    CHECK_EQ(rx(1, 0, 1, 100, 4, 50, SEC, false), SACN_RX_OUTPUT);
    int len = sacn_pkt(pkt, 1, 1, 1, 100, 4, 0);
    pkt[112] = 0x40;                                    /* Stream_Terminated */
    CHECK_EQ(sacn_core_dmx(&M, pkt, len, SEC + 1, 1, false, out), SACN_RX_TERMINATED);
    CHECK_EQ(sacn_core_active(&M, SEC + 1), 0);
    CHECK_EQ(out[0], 50);                               /* output untouched */
    CHECK_EQ(sacn_core_dmx(&M, pkt, len, SEC + 2, 1, false, out), SACN_RX_IGNORED);   /* unknown */

    reset();                                            /* the other source takes over */
    CHECK_EQ(rx(1, 0, 1, 100, 4, 50, SEC, false), SACN_RX_OUTPUT);
    CHECK_EQ(rx(2, 0, 1, 100, 4, 20, SEC + 1, false), SACN_RX_OUTPUT);
    len = sacn_pkt(pkt, 1, 1, 1, 100, 4, 0);
    pkt[112] = 0x40;
    CHECK_EQ(sacn_core_dmx(&M, pkt, len, SEC + 2, 1, false, out), SACN_RX_OUTPUT);
    CHECK_EQ(out[0], 20);
    CHECK(!M.merging);
}

static void test_sacn_helpers(void)
{
    CHECK_EQ(sacn_multicast_addr(1), 0xEFFF0001u);
    CHECK_EQ(sacn_multicast_addr(0x1234), 0xEFFF1234u);
    CHECK(!sacn_universe_valid(0));
    CHECK(sacn_universe_valid(1));
    CHECK(sacn_universe_valid(63999));
    CHECK(!sacn_universe_valid(64000));
}

void test_sacn(void)
{
    test_sacn_basic();
    test_sacn_malformed();
    test_sacn_sequence();
    test_sacn_merge();
    test_sacn_priority();
    test_sacn_terminate();
    test_sacn_helpers();
}
