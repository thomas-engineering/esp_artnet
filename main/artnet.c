#include "artnet.h"
#include "artnet_poll.h"
#include "artnet_ip.h"
#include "artnet_core.h"
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
#include "esp_random.h"
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
_Static_assert(ARTNET_CORE_CHANNELS == DMX_CHANNELS, "channel count mismatch");

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

/* "Data is being transmitted" in ArtPollReply: ArtDmx seen within this time */
#define ACTIVE_US            (3 * 1000000LL)

#define NVS_NAMESPACE     "artnet"
#define NVS_KEY_FAILSAFE  "failsafe"
#define NVS_KEY_REMOTE    "remote_prog"

static artnet_merge_t   s_merge;                 /* sources + merge state (artnet_core) */
static uint8_t           s_buf[DMX_CHANNELS];   /* merged output, read by the DMX task */
static SemaphoreHandle_t s_mutex;               /* protects s_buf */
static volatile uint16_t s_universe;
static volatile uint32_t s_pkt_count = 0;
static volatile uint32_t s_pps = 0;
static volatile int64_t  s_last_dmx_us = 0;     /* 0 = no ArtDmx received yet */
static volatile uint8_t  s_failsafe = ARTNET_FAILSAFE_HOLD;
static bool              s_failsafe_applied = false;
static volatile bool     s_remote_prog = true;  /* ArtAddress / ArtIpProg allowed */

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

/* ArtPoll replies are delayed by a random 0-1 s (Art-Net 4 recommendation),
   so that many nodes answering one broadcast poll do not collide. */
#define POLL_DELAY_MAX_MS  1000
#define POLL_QUEUE_LEN     4

typedef struct {
    struct sockaddr_in dst;
    int64_t            due_us;
    bool               used;
} poll_pending_t;

static poll_pending_t s_poll_q[POLL_QUEUE_LEN];

static void poll_reply_schedule(int sock, uint8_t *reply, const struct sockaddr_in *src, int64_t now)
{
    poll_pending_t *free_slot = NULL;
    for (int i = 0; i < POLL_QUEUE_LEN; i++) {
        poll_pending_t *q = &s_poll_q[i];
        if (q->used && q->dst.sin_addr.s_addr == src->sin_addr.s_addr &&
            q->dst.sin_port == src->sin_port)
            return;                             /* this controller is already waiting */
        if (!q->used && !free_slot) free_slot = q;
    }
    if (!free_slot) {                           /* queue full: answer at once */
        send_reply(sock, reply, artnet_build_poll_reply(reply, s_universe), src);
        return;
    }
    free_slot->dst    = *src;
    free_slot->due_us = now + (int64_t)(esp_random() % POLL_DELAY_MAX_MS) * 1000;
    free_slot->used   = true;
}

static void poll_reply_flush(int sock, uint8_t *reply, int64_t now)
{
    for (int i = 0; i < POLL_QUEUE_LEN; i++) {
        poll_pending_t *q = &s_poll_q[i];
        if (q->used && now >= q->due_us) {
            q->used = false;
            send_reply(sock, reply, artnet_build_poll_reply(reply, s_universe), &q->dst);
        }
    }
}

/* ── ArtDmx ───────────────────────────────────────────────────────────── */

static void handle_art_dmx(const uint8_t *p, int len, uint32_t src_ip)
{
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = artnet_core_dmx(&s_merge, p, len, src_ip, now, s_universe, s_buf);
    xSemaphoreGive(s_mutex);
    if (!ok) return;

    s_last_dmx_us      = now;
    s_failsafe_applied = false;
    led_artnet_pulse();
    s_pkt_count++;
}

/* Data loss: after FAILSAFE_TIMEOUT_US without ArtDmx apply the failsafe mode */
static void check_failsafe(int64_t now)
{
    if (!artnet_core_failsafe_due(s_failsafe, s_failsafe_applied, s_last_dmx_us, now)) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_buf, s_failsafe == ARTNET_FAILSAFE_FULL ? 255 : 0, DMX_CHANNELS);
    xSemaphoreGive(s_mutex);
    s_failsafe_applied = true;
    s_merge.merging = false;
    ESP_LOGW(TAG, "No Art-Net data: failsafe output (all %s)",
             s_failsafe == ARTNET_FAILSAFE_FULL ? "full" : "zero");
}

/* ── ArtPoll ──────────────────────────────────────────────────────────── */

/* Returns true if the node has to answer this ArtPoll */
static bool handle_art_poll(const uint8_t *p, int len)
{
    uint8_t flags = len >= 13 ? p[12] : 0;
    s_talk_on_change = (flags & ARTPOLL_TALK_ON_CHANGE) != 0;
    return artnet_core_poll_matches(p, len, s_universe);
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

    uint16_t u = artnet_core_address_port(p, s_universe);
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
    case AC_CANCEL_MERGE: s_merge.cancel_merge = true; break;
    case AC_LED_NORMAL: led_set_mode(LED_MODE_NORMAL); artnet_announce(); break;
    case AC_LED_MUTE:   led_set_mode(LED_MODE_MUTE);   artnet_announce(); break;
    case AC_LED_LOCATE: led_set_mode(LED_MODE_LOCATE); artnet_announce(); break;
    case AC_FAIL_HOLD:  artnet_set_failsafe(ARTNET_FAILSAFE_HOLD); break;
    case AC_FAIL_ZERO:  artnet_set_failsafe(ARTNET_FAILSAFE_ZERO); break;
    case AC_FAIL_FULL:  artnet_set_failsafe(ARTNET_FAILSAFE_FULL); break;
    case AC_MERGE_LTP0: s_merge.merge_ltp = true;  artnet_announce(); break;
    case AC_MERGE_HTP0: s_merge.merge_ltp = false; artnet_announce(); break;
    case AC_CLEAR_OP0:
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(s_buf, 0, DMX_CHANNELS);
        xSemaphoreGive(s_mutex);
        artnet_core_clear(&s_merge);
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

    /* Wake up regularly: announcements, delayed poll replies, failsafe and
       the packet rate need it */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
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
        poll_reply_flush(sock, reply, now);
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
            handle_art_dmx(raw, len, src.sin_addr.s_addr);
            break;

        case ARTNET_OP_POLL:
            if (handle_art_poll(raw, len))
                poll_reply_schedule(sock, reply, &src, now);
            break;

        case ARTNET_OP_ADDRESS:
            if (len >= ARTADDRESS_LEN) {
                if (s_remote_prog) handle_art_address(raw);
                else ESP_LOGW(TAG, "ArtAddress ignored: remote programming is off");
                size_t n = artnet_build_poll_reply(reply, s_universe);
                send_reply(sock, reply, n, &src);
            }
            break;

        case ARTNET_OP_IPPROG: {
            static uint8_t ip_reply[ARTNET_IPPROG_REPLY_LEN];
            bool restart;
            if (!s_remote_prog) ESP_LOGW(TAG, "ArtIpProg ignored: remote programming is off");
            /* len 0 = no command: the reply only reports the current settings */
            size_t n = artnet_ip_prog(raw, s_remote_prog ? len : 0, ip_reply, &restart);
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
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t rp = 1;
        nvs_get_u8(h, NVS_KEY_REMOTE, &rp);
        nvs_close(h);
        s_remote_prog = rp != 0;
    }

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

bool artnet_get_remote_prog(void) { return s_remote_prog; }

void artnet_set_remote_prog(bool allow)
{
    if (allow == s_remote_prog) return;
    s_remote_prog = allow;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_REMOTE, allow ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Remote programming %s", allow ? "allowed" : "blocked");
}

uint8_t artnet_good_output(void)
{
    uint8_t v = 0;
    if (artnet_idle_ms() < ACTIVE_US / 1000) v |= 0x80;    /* data is being transmitted */
    if (s_merge.merging && (v & 0x80))             v |= 0x08;    /* merging two sources       */
    if (s_merge.merge_ltp)                   v |= 0x02;    /* merge mode is LTP         */
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
