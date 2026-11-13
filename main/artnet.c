#include "artnet.h"
#include "artnet_poll.h"
#include "artnet_ip.h"
#include "provisioning.h"
#include "dmx.h"
#include "led.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "artnet";

/* Art-Net ArtDmx packet layout (all multi-byte fields little-endian except ProtVer) */
typedef struct __attribute__((packed)) {
    uint8_t  id[8];         /* "Art-Net\0" */
    uint16_t opcode;        /* 0x5000 = ArtDmx (LE) */
    uint16_t prot_ver;      /* 14 (BE) */
    uint8_t  sequence;
    uint8_t  physical;
    uint16_t universe;      /* 15-bit port-address (LE) */
    uint16_t length;        /* channel count, big-endian */
    uint8_t  data[DMX_CHANNELS];
} artnet_dmx_t;

#define ARTNET_ID         "Art-Net\0"
#define ARTNET_PROT_VER   14
#define ARTNET_OP_DMX     0x5000u
#define ARTNET_OP_POLL    0x2000u /* ArtPoll: controller looks for nodes */
#define ARTNET_OP_ADDRESS 0x6000u /* ArtAddress: remote programming of the node */
#define ARTNET_OP_IPPROG  0xF800u /* ArtIpProg: remote IP configuration */
#define ARTDMX_HEADER_LEN 18

/* ArtAddress command values (Command field, byte 106) */
#define AC_CANCEL_MERGE 0x01
#define AC_LED_NORMAL   0x02
#define AC_LED_MUTE     0x03
#define AC_LED_LOCATE   0x04
#define AC_FAIL_HOLD    0x08
#define AC_FAIL_ZERO    0x09
#define AC_FAIL_FULL    0x0A
#define AC_MERGE_LTP0   0x10      /* port 0: merge in LTP mode */
#define AC_MERGE_HTP0   0x50      /* port 0: merge in HTP mode (default) */
#define AC_CLEAR_OP0    0x90      /* clear the DMX output buffer of port 0 */

#define ARTADDRESS_LEN  107

/* ArtPoll flags (TalkToMe, byte 12) */
#define ARTPOLL_TALK_ON_CHANGE 0x02   /* send ArtPollReply whenever the node changes */
#define ARTPOLL_TARGETED       0x20   /* only nodes inside the target range answer   */

/* A source that has been silent this long no longer takes part in merging */
#define MERGE_TIMEOUT_US     (10 * 1000000LL)
/* After this long without ArtDmx the sequence check starts afresh */
#define SEQ_RESET_US         (1 * 1000000LL)
/* No ArtDmx for this long = data loss: apply the failsafe setting */
#define FAILSAFE_TIMEOUT_US  (6 * 1000000LL)
/* "Data is being transmitted" in ArtPollReply: ArtDmx seen within this time */
#define ACTIVE_US            (3 * 1000000LL)

#define NVS_NAMESPACE     "artnet"
#define NVS_KEY_FAILSAFE  "failsafe"

/* Up to two controllers may send to the universe; their data is merged */
typedef struct {
    uint32_t ip;                    /* network byte order, 0 = slot unused */
    int64_t  last_us;               /* time of the last accepted packet   */
    uint8_t  seq;                   /* last sequence number (0 = none)    */
    uint8_t  data[DMX_CHANNELS];    /* last frame of this source          */
} dmx_source_t;

static dmx_source_t      s_src[2];
static uint8_t           s_buf[DMX_CHANNELS];   /* merged output, read by the DMX task */
static SemaphoreHandle_t s_mutex;               /* protects s_buf */
static volatile uint16_t s_universe;
static volatile uint32_t s_pkt_count = 0;
static volatile uint32_t s_pps = 0;
static volatile int64_t  s_last_dmx_us = 0;     /* 0 = no ArtDmx received yet */
static volatile bool     s_merging = false;
static volatile bool     s_merge_ltp = false;   /* false = HTP */
static volatile bool     s_cancel_merge = false;
static volatile uint8_t  s_failsafe = ARTNET_FAILSAFE_HOLD;
static bool              s_failsafe_applied = false;

/* Unsolicited ArtPollReply broadcasts: sent once at start and, when a
   controller asked for it via ArtPoll TalkToMe, whenever the node changes. */
static volatile bool s_announce_pending = false;
static volatile bool s_announce_force   = true;    /* announce once at boot */
static volatile bool s_talk_on_change   = false;

void artnet_announce(void)
{
    s_announce_pending = true;
}

/* The new IP configuration takes effect after a reboot; give the reply time to leave */
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

/* ── Sending ──────────────────────────────────────────────────────────── */

/* Sends a reply to a controller: to the Art-Net port of its address, as the
   specification requires, and also to the port it sent from when that is a
   different one (tools that poll from an ephemeral port). */
static void send_reply(int sock, const uint8_t *buf, size_t n, const struct sockaddr_in *src)
{
    struct sockaddr_in dst = *src;
    dst.sin_port = htons(ARTNET_PORT);
    sendto(sock, buf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (src->sin_port != dst.sin_port)
        sendto(sock, buf, n, 0, (const struct sockaddr *)src, sizeof(*src));
}

/* Broadcasts an ArtPollReply to the subnet's directed broadcast address */
static void send_broadcast_reply(int sock, uint8_t *reply)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return;

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ARTNET_PORT),
        .sin_addr.s_addr = info.ip.addr | ~info.netmask.addr,
    };
    size_t n = artnet_build_poll_reply(reply, s_universe);
    sendto(sock, reply, n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ── ArtDmx ───────────────────────────────────────────────────────────── */

static bool source_active(const dmx_source_t *s, int64_t now)
{
    return s->ip != 0 && now - s->last_us < MERGE_TIMEOUT_US;
}

static void handle_art_dmx(const artnet_dmx_t *pkt, int len, uint32_t src_ip)
{
    if (len <= ARTDMX_HEADER_LEN) return;
    if (ntohs(pkt->prot_ver) < ARTNET_PROT_VER) return;
    if (pkt->universe != s_universe) return;    /* LE field on an LE machine */

    /* Never trust the length field beyond what was actually received */
    uint16_t data_len = ntohs(pkt->length);
    if (data_len > len - ARTDMX_HEADER_LEN) data_len = len - ARTDMX_HEADER_LEN;
    if (data_len > DMX_CHANNELS) data_len = DMX_CHANNELS;
    if (data_len == 0) return;

    int64_t now = esp_timer_get_time();

    /* Find the slot of this source, or take a free / expired one */
    dmx_source_t *slot = NULL;
    for (int i = 0; i < 2; i++) {
        if (s_src[i].ip == src_ip && source_active(&s_src[i], now)) slot = &s_src[i];
    }
    if (!slot) {
        for (int i = 0; i < 2 && !slot; i++) {
            if (!source_active(&s_src[i], now)) {
                slot = &s_src[i];
                memset(slot, 0, sizeof(*slot));
                slot->ip = src_ip;
            }
        }
    }
    if (!slot) return;                          /* a third source is ignored */
    dmx_source_t *other = (slot == &s_src[0]) ? &s_src[1] : &s_src[0];

    /* Sequence check: drop packets that arrive out of order. 0 disables it;
       after a pause the sender may have restarted, so start afresh. */
    uint8_t seq = pkt->sequence;
    if (seq != 0 && slot->seq != 0 && now - slot->last_us < SEQ_RESET_US &&
        (uint8_t)(seq - slot->seq) >= 0x80) {
        return;
    }
    slot->seq = seq;

    if (s_cancel_merge) {                       /* AcCancelMerge: this source wins */
        s_cancel_merge = false;
        other->ip = 0;
    }
    bool merging = source_active(other, now);
    s_merging = merging;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!merging) {
        memcpy(s_buf, pkt->data, data_len);
    } else if (s_merge_ltp) {
        /* LTP: a channel follows whichever source changed it last */
        for (uint16_t i = 0; i < data_len; i++)
            if (pkt->data[i] != slot->data[i]) s_buf[i] = pkt->data[i];
    } else {
        /* HTP: the highest level of both sources wins */
        for (uint16_t i = 0; i < data_len; i++)
            s_buf[i] = pkt->data[i] > other->data[i] ? pkt->data[i] : other->data[i];
    }
    xSemaphoreGive(s_mutex);

    memcpy(slot->data, pkt->data, data_len);
    slot->last_us      = now;
    s_last_dmx_us      = now;
    s_failsafe_applied = false;
    led_artnet_pulse();
    s_pkt_count++;
}

/* Data loss: after FAILSAFE_TIMEOUT_US without ArtDmx apply the failsafe mode */
static void check_failsafe(int64_t now)
{
    if (s_failsafe == ARTNET_FAILSAFE_HOLD || s_failsafe_applied) return;
    if (s_last_dmx_us == 0 || now - s_last_dmx_us < FAILSAFE_TIMEOUT_US) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_buf, s_failsafe == ARTNET_FAILSAFE_FULL ? 255 : 0, DMX_CHANNELS);
    xSemaphoreGive(s_mutex);
    s_failsafe_applied = true;
    s_merging = false;
    ESP_LOGW(TAG, "No Art-Net data: failsafe output (all %s)",
             s_failsafe == ARTNET_FAILSAFE_FULL ? "full" : "zero");
}

/* ── ArtPoll ──────────────────────────────────────────────────────────── */

/* Returns true if the node has to answer this ArtPoll */
static bool handle_art_poll(const uint8_t *p, int len)
{
    uint8_t flags = len >= 13 ? p[12] : 0;
    s_talk_on_change = (flags & ARTPOLL_TALK_ON_CHANGE) != 0;

    /* Targeted mode: only answer if our port-address is inside the range */
    if ((flags & ARTPOLL_TARGETED) && len >= 18) {
        uint16_t top    = ((uint16_t)p[14] << 8) | p[15];
        uint16_t bottom = ((uint16_t)p[16] << 8) | p[17];
        if (s_universe < bottom || s_universe > top) return false;
    }
    return true;
}

/* ── ArtAddress ───────────────────────────────────────────────────────── */

/* ArtAddress: remote programming (the caller answers with an ArtPollReply).
   Fields follow the Art-Net 4 layout: NetSwitch@12, BindIndex@13, PortName@14,
   LongName@32, SwOut@100, SubSwitch@104, Command@106. Address bytes are only
   applied when bit 7 is set (value 0x80 | n); 0x7F and 0x00 mean "no change" -
   the node has no physical switches to fall back to. */
static void handle_art_address(const uint8_t *p)
{
    if (p[13] > 1) return;                      /* BindIndex: only our single port */

    uint16_t u = s_universe;
    if (p[12]  & 0x80) u = (u & 0x00FF) | ((uint16_t)(p[12]  & 0x7F) << 8);   /* net      */
    if (p[104] & 0x80) u = (u & 0x7F0F) | ((uint16_t)(p[104] & 0x0F) << 4);   /* sub-net  */
    if (p[100] & 0x80) u = (u & 0x7FF0) |  (uint16_t)(p[100] & 0x0F);         /* universe */
    if (u != s_universe) {
        provisioning_save_universe(u);
        artnet_set_universe(u);                 /* also schedules an announcement */
        ESP_LOGI(TAG, "ArtAddress: port-address set to %u", u);
    }

    char sname[ARTNET_SHORT_NAME_LEN], lname[ARTNET_LONG_NAME_LEN];
    memcpy(sname, p + 14, sizeof(sname));  sname[sizeof(sname) - 1] = '\0';
    memcpy(lname, p + 32, sizeof(lname));  lname[sizeof(lname) - 1] = '\0';
    if (artnet_names_set(sname, lname)) {
        ESP_LOGI(TAG, "ArtAddress: names updated");
        artnet_announce();
    }

    switch (p[106]) {
    case AC_CANCEL_MERGE: s_cancel_merge = true; break;
    case AC_LED_NORMAL: led_set_mode(LED_MODE_NORMAL); artnet_announce(); break;
    case AC_LED_MUTE:   led_set_mode(LED_MODE_MUTE);   artnet_announce(); break;
    case AC_LED_LOCATE: led_set_mode(LED_MODE_LOCATE); artnet_announce(); break;
    case AC_FAIL_HOLD:  artnet_set_failsafe(ARTNET_FAILSAFE_HOLD); break;
    case AC_FAIL_ZERO:  artnet_set_failsafe(ARTNET_FAILSAFE_ZERO); break;
    case AC_FAIL_FULL:  artnet_set_failsafe(ARTNET_FAILSAFE_FULL); break;
    case AC_MERGE_LTP0: s_merge_ltp = true;  artnet_announce(); break;
    case AC_MERGE_HTP0: s_merge_ltp = false; artnet_announce(); break;
    case AC_CLEAR_OP0:
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(s_buf, 0, DMX_CHANNELS);
        xSemaphoreGive(s_mutex);
        memset(s_src[0].data, 0, DMX_CHANNELS);
        memset(s_src[1].data, 0, DMX_CHANNELS);
        ESP_LOGI(TAG, "ArtAddress: output cleared");
        break;
    default: break;
    }
}

/* ── Receive task ─────────────────────────────────────────────────────── */

static void artnet_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ARTNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Wake up regularly: announcements, failsafe and the packet rate need it */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Listening for Art-Net on UDP port %d, universe %d",
             ARTNET_PORT, s_universe);

    static artnet_dmx_t pkt;
    static uint8_t      reply[ARTNET_POLL_REPLY_LEN];
    const uint8_t      *raw = (const uint8_t *)&pkt;
    int64_t  pps_t   = esp_timer_get_time();
    uint32_t pps_cnt = 0;

    while (1) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, &pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&src, &src_len);
        int64_t now = esp_timer_get_time();

        if (s_announce_pending && (s_announce_force || s_talk_on_change)) {
            s_announce_pending = false;
            s_announce_force   = false;
            send_broadcast_reply(sock, reply);
        }
        check_failsafe(now);
        if (now - pps_t >= 1000000) {           /* packets per second, once a second */
            uint32_t cnt = s_pkt_count;
            s_pps   = (uint32_t)((uint64_t)(cnt - pps_cnt) * 1000000ULL / (uint64_t)(now - pps_t));
            pps_cnt = cnt;
            pps_t   = now;
        }

        if (len < 12) continue;                 /* ArtPoll is the shortest packet */
        if (memcmp(pkt.id, ARTNET_ID, 8) != 0) continue;

        switch (pkt.opcode) {
        case ARTNET_OP_DMX:
            handle_art_dmx(&pkt, len, src.sin_addr.s_addr);
            break;

        case ARTNET_OP_POLL:
            if (handle_art_poll(raw, len)) {
                size_t n = artnet_build_poll_reply(reply, s_universe);
                send_reply(sock, reply, n, &src);
            }
            break;

        case ARTNET_OP_ADDRESS:
            if (len >= ARTADDRESS_LEN) {
                handle_art_address(raw);
                size_t n = artnet_build_poll_reply(reply, s_universe);
                send_reply(sock, reply, n, &src);
            }
            break;

        case ARTNET_OP_IPPROG: {
            static uint8_t ip_reply[ARTNET_IPPROG_REPLY_LEN];
            bool restart;
            size_t n = artnet_ip_prog(raw, len, ip_reply, &restart);
            send_reply(sock, ip_reply, n, &src);
            if (restart) xTaskCreate(restart_task, "restart", 3072, NULL, 1, NULL);
            break;
        }

        default:
            break;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void artnet_start(uint16_t universe)
{
    s_universe = universe;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    nvs_handle_t h;
    uint8_t fs = ARTNET_FAILSAFE_HOLD;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_FAILSAFE, &fs);
        nvs_close(h);
    }
    s_failsafe = fs <= ARTNET_FAILSAFE_FULL ? fs : ARTNET_FAILSAFE_HOLD;

    artnet_poll_init();
    xTaskCreate(artnet_task, "artnet", 4096, NULL, 5, NULL);
}

uint32_t artnet_get_pkt_count(void) { return s_pkt_count; }
uint32_t artnet_get_pps(void)       { return s_pps; }
uint16_t artnet_get_universe(void)  { return s_universe; }

uint32_t artnet_idle_ms(void)
{
    int64_t last = s_last_dmx_us;
    if (last == 0) return UINT32_MAX;
    int64_t ms = (esp_timer_get_time() - last) / 1000;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

void artnet_set_universe(uint16_t universe)
{
    if (universe != s_universe) {
        s_universe = universe;
        artnet_announce();
    }
}

artnet_failsafe_t artnet_get_failsafe(void) { return (artnet_failsafe_t)s_failsafe; }

void artnet_set_failsafe(artnet_failsafe_t mode)
{
    if (mode > ARTNET_FAILSAFE_FULL || mode == s_failsafe) return;
    s_failsafe = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_FAILSAFE, (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Failsafe mode: %s",
             mode == ARTNET_FAILSAFE_HOLD ? "hold" : mode == ARTNET_FAILSAFE_ZERO ? "zero" : "full");
    artnet_announce();
}

uint8_t artnet_good_output(void)
{
    uint8_t v = 0;
    if (artnet_idle_ms() < ACTIVE_US / 1000) v |= 0x80;    /* data is being transmitted */
    if (s_merging && (v & 0x80))             v |= 0x08;    /* merging two sources       */
    if (s_merge_ltp)                         v |= 0x02;    /* merge mode is LTP         */
    return v;
}

void artnet_copy_dmx(uint8_t *dst, uint16_t max_len)
{
    uint16_t n = max_len < DMX_CHANNELS ? max_len : DMX_CHANNELS;
    if (!s_mutex) {                             /* DMX output runs before Art-Net starts */
        memset(dst, 0, n);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(dst, s_buf, n);
    xSemaphoreGive(s_mutex);
}
