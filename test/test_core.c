#include "test.h"
#include "artnet_core.h"
#include <string.h>

#define SEC 1000000LL

/* Builds an ArtDmx packet; returns its length. */
static int dmx_pkt(uint8_t *p, uint8_t seq, uint16_t universe, int declared_len,
                   int sent_len, uint8_t fill)
{
    memset(p, 0, 18 + 512);
    memcpy(p, "Art-Net\0", 8);
    p[8] = 0x00; p[9] = 0x50;                   /* OpCode 0x5000 (LE) */
    p[10] = 0; p[11] = 14;                      /* ProtVer (BE) */
    p[12] = seq;
    p[14] = universe & 0xFF; p[15] = universe >> 8;   /* LE */
    p[16] = declared_len >> 8; p[17] = declared_len & 0xFF;   /* BE */
    memset(p + 18, fill, sent_len);
    return 18 + sent_len;
}

static artnet_merge_t M;
static uint8_t out[512], pkt[18 + 512];

static void reset(void) { memset(&M, 0, sizeof(M)); memset(out, 0, sizeof(out)); }

static int rx(uint8_t seq, uint16_t uni, int decl, int sent, uint8_t fill,
              uint32_t ip, int64_t now)
{
    int len = dmx_pkt(pkt, seq, uni, decl, sent, fill);
    return artnet_core_dmx(&M, pkt, len, ip, now, 0, out) ? 1 : 0;
}

static void test_dmx_basic(void)
{
    reset();
    CHECK_EQ(rx(1, 0, 512, 512, 77, 1, SEC), 1);
    CHECK_EQ(out[0], 77); CHECK_EQ(out[511], 77);
    CHECK(!M.merging);

    reset();
    CHECK_EQ(rx(1, 5, 512, 512, 77, 1, SEC), 0);            /* wrong universe */
    CHECK_EQ(out[0], 0);

    reset();
    int len = dmx_pkt(pkt, 1, 0, 512, 512, 9);
    pkt[11] = 13;                                           /* ProtVer 13 */
    CHECK(!artnet_core_dmx(&M, pkt, len, 1, SEC, 0, out));
    CHECK(!artnet_core_dmx(&M, pkt, 18, 1, SEC, 0, out));   /* header only */
    CHECK(!artnet_core_dmx(&M, pkt, 5, 1, SEC, 0, out));

    reset();
    CHECK_EQ(rx(1, 0, 0, 0, 0, 1, SEC), 0);                 /* length 0 */
}

static void test_dmx_length(void)
{
    /* Declared 512 but only 10 bytes received: no stale bytes after slot 10 */
    reset();
    memset(out, 0xEE, sizeof(out));
    CHECK_EQ(rx(1, 0, 512, 10, 5, 1, SEC), 1);
    CHECK_EQ(out[9], 5);
    CHECK_EQ(out[10], 0xEE);

    /* Declared shorter than received: only the declared slots */
    reset();
    memset(out, 0xEE, sizeof(out));
    CHECK_EQ(rx(1, 0, 4, 100, 5, 1, SEC), 1);
    CHECK_EQ(out[3], 5);
    CHECK_EQ(out[4], 0xEE);

    /* Declared above 512 is clamped */
    reset();
    CHECK_EQ(rx(1, 0, 600, 512, 7, 1, SEC), 1);
    CHECK_EQ(out[511], 7);
}

static void test_sequence(void)
{
    reset();
    CHECK_EQ(rx(10, 0, 512, 512, 1, 1, SEC), 1);
    CHECK_EQ(rx(11, 0, 512, 512, 2, 1, SEC + 20000), 1);
    CHECK_EQ(rx(9, 0, 512, 512, 3, 1, SEC + 40000), 0);     /* older: dropped */
    CHECK_EQ(out[0], 2);
    CHECK_EQ(rx(12, 0, 512, 512, 4, 1, SEC + 60000), 1);    /* newer than last accepted */
    CHECK_EQ(out[0], 4);

    /* wrap-around 255 -> 1 is "newer" */
    reset();
    CHECK_EQ(rx(255, 0, 512, 512, 1, 1, SEC), 1);
    CHECK_EQ(rx(1, 0, 512, 512, 2, 1, SEC + 20000), 1);

    /* sequence 0 = check disabled */
    reset();
    CHECK_EQ(rx(100, 0, 512, 512, 1, 1, SEC), 1);
    CHECK_EQ(rx(0, 0, 512, 512, 2, 1, SEC + 20000), 1);

    /* after a pause (> 1 s) an "older" number means the sender restarted */
    reset();
    CHECK_EQ(rx(200, 0, 512, 512, 1, 1, SEC), 1);
    CHECK_EQ(rx(190, 0, 512, 512, 2, 1, SEC + SEC / 2), 0);
    CHECK_EQ(rx(190, 0, 512, 512, 3, 1, SEC + SEC + 1000), 1);
}

static void test_merge(void)
{
    /* two sources, HTP: highest level wins per channel */
    reset();
    CHECK_EQ(rx(1, 0, 512, 512, 100, 1, SEC), 1);
    CHECK_EQ(rx(1, 0, 512, 512, 50, 2, SEC + 1000), 1);
    CHECK(M.merging);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(rx(2, 0, 512, 512, 200, 2, SEC + 2000), 1);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(rx(2, 0, 512, 512, 10, 1, SEC + 3000), 1);     /* A drops, B still 200 */
    CHECK_EQ(out[0], 200);

    /* a third source is ignored */
    CHECK_EQ(rx(1, 0, 512, 512, 255, 3, SEC + 4000), 0);

    /* LTP: the source that changed a channel last wins */
    reset();
    M.merge_ltp = true;
    CHECK_EQ(rx(1, 0, 512, 512, 100, 1, SEC), 1);
    CHECK_EQ(rx(1, 0, 512, 512, 50, 2, SEC + 1000), 1);
    CHECK_EQ(out[0], 50);
    CHECK_EQ(rx(2, 0, 512, 512, 100, 1, SEC + 2000), 1);    /* A unchanged: B stays */
    CHECK_EQ(out[0], 50);
    CHECK_EQ(rx(3, 0, 512, 512, 30, 1, SEC + 3000), 1);     /* A changes: A wins */
    CHECK_EQ(out[0], 30);

    /* a silent source (> 10 s) leaves the merge */
    reset();
    CHECK_EQ(rx(1, 0, 512, 512, 200, 1, SEC), 1);
    CHECK_EQ(rx(1, 0, 512, 512, 10, 2, 2 * SEC), 1);
    CHECK_EQ(out[0], 200);
    CHECK_EQ(rx(2, 0, 512, 512, 10, 2, 13 * SEC), 1);
    CHECK(!M.merging);
    CHECK_EQ(out[0], 10);

    /* AcCancelMerge: the next source wins, the other is dropped */
    reset();
    CHECK_EQ(rx(1, 0, 512, 512, 200, 1, SEC), 1);
    CHECK_EQ(rx(1, 0, 512, 512, 10, 2, SEC + 1000), 1);
    M.cancel_merge = true;
    CHECK_EQ(rx(2, 0, 512, 512, 10, 2, SEC + 2000), 1);
    CHECK(!M.merging);
    CHECK_EQ(out[0], 10);

    /* AcClearOp */
    artnet_core_clear(&M);
    CHECK_EQ(M.src[0].data[0], 0);
    CHECK_EQ(M.src[1].data[0], 0);
}

static void test_failsafe(void)
{
    CHECK(!artnet_core_failsafe_due(0, false, SEC, 100 * SEC));        /* hold: never */
    CHECK(!artnet_core_failsafe_due(1, false, 0, 100 * SEC));          /* no data yet */
    CHECK(!artnet_core_failsafe_due(1, false, SEC, 6 * SEC));          /* 5 s: too early */
    CHECK(artnet_core_failsafe_due(1, false, SEC, 7 * SEC));           /* 6 s */
    CHECK(artnet_core_failsafe_due(2, false, SEC, 8 * SEC));
    CHECK(!artnet_core_failsafe_due(1, true, SEC, 100 * SEC));         /* only once */
}

static void test_poll_and_address(void)
{
    uint8_t p[32] = {0};
    CHECK(artnet_core_poll_matches(p, 14, 7));                         /* untargeted */

    p[12] = 0x20;                                                      /* targeted */
    p[14] = 0; p[15] = 10;                                             /* top = 10 */
    p[16] = 0; p[17] = 2;                                              /* bottom = 2 */
    CHECK(artnet_core_poll_matches(p, 18, 2));
    CHECK(artnet_core_poll_matches(p, 18, 10));
    CHECK(!artnet_core_poll_matches(p, 18, 1));
    CHECK(!artnet_core_poll_matches(p, 18, 11));
    CHECK(artnet_core_poll_matches(p, 14, 99));                        /* too short: answer */

    uint8_t a[107] = {0};
    CHECK_EQ(artnet_core_address_port(a, 0x1234), 0x1234);             /* nothing set */
    a[12] = 0x80 | 5;                                                  /* net 5 */
    CHECK_EQ(artnet_core_address_port(a, 0x0123), 0x0523);
    memset(a, 0, sizeof(a)); a[104] = 0x80 | 0x0A;                     /* sub-net 10 */
    CHECK_EQ(artnet_core_address_port(a, 0x0123), 0x01A3);
    memset(a, 0, sizeof(a)); a[100] = 0x80 | 0x07;                     /* universe 7 */
    CHECK_EQ(artnet_core_address_port(a, 0x0123), 0x0127);
    memset(a, 0, sizeof(a)); a[12] = 0x7F; a[104] = 0x7F; a[100] = 0x00; /* no bit 7 */
    CHECK_EQ(artnet_core_address_port(a, 0x0123), 0x0123);
    memset(a, 0, sizeof(a)); a[12] = 0xFF; a[104] = 0x8F; a[100] = 0x8F;
    CHECK_EQ(artnet_core_address_port(a, 0), 0x7FFF);
}

void test_core(void)
{
    test_dmx_basic();
    test_dmx_length();
    test_sequence();
    test_merge();
    test_failsafe();
    test_poll_and_address();
}
