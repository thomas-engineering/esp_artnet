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

#define ARTNET_ID       "Art-Net\0"
#define ARTNET_OP_DMX   0x5000u
#define ARTNET_OP_POLL  0x2000u   /* ArtPoll: controller looks for nodes */
#define ARTNET_OP_ADDRESS 0x6000u /* ArtAddress: remote programming of the node */
#define ARTNET_OP_IPPROG  0xF800u /* ArtIpProg: remote IP configuration */

/* ArtAddress command values (Command field, byte 106) */
#define AC_LED_NORMAL   0x02
#define AC_LED_MUTE     0x03
#define AC_LED_LOCATE   0x04
#define AC_CLEAR_OP0    0x90      /* clear the DMX output buffer of port 0 */

#define ARTADDRESS_LEN  107
#define ARTPOLL_TALK_ON_CHANGE 0x02   /* TalkToMe bit 1: send replies on change */

static uint8_t           s_buf[DMX_CHANNELS];
static SemaphoreHandle_t s_mutex;
static uint16_t          s_universe;
static volatile uint32_t s_pkt_count = 0;

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

/* ArtAddress: remote programming. Returns true if the node was reprogrammed
   (the caller answers with an ArtPollReply in any case). Fields follow the
   Art-Net 4 layout: NetSwitch@12, BindIndex@13, PortName@14, LongName@32,
   SwOut@100, SubSwitch@104, Command@106. Address bytes are only applied when
   bit 7 is set (value 0x80 | n); 0x7F and 0x00 mean "no change" - the node has
   no physical switches to fall back to. */
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

    char sname[18], lname[64];
    memcpy(sname, p + 14, sizeof(sname));  sname[sizeof(sname) - 1] = '\0';
    memcpy(lname, p + 32, sizeof(lname));  lname[sizeof(lname) - 1] = '\0';
    if (artnet_names_set(sname, lname)) {
        ESP_LOGI(TAG, "ArtAddress: names updated");
        artnet_announce();
    }

    switch (p[106]) {
    case AC_LED_NORMAL: led_set_mode(LED_MODE_NORMAL); artnet_announce(); break;
    case AC_LED_MUTE:   led_set_mode(LED_MODE_MUTE);   artnet_announce(); break;
    case AC_LED_LOCATE: led_set_mode(LED_MODE_LOCATE); artnet_announce(); break;
    case AC_CLEAR_OP0:
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(s_buf, 0, DMX_CHANNELS);
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "ArtAddress: output cleared");
        break;
    default: break;                             /* no merge / failsafe features */
    }
}

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

    /* Wake up regularly so pending announcements are sent promptly */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Listening for Art-Net on UDP port %d, universe %d",
             ARTNET_PORT, s_universe);

    static artnet_dmx_t pkt;
    static uint8_t      reply[ARTNET_POLL_REPLY_LEN];
    while (1) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, &pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&src, &src_len);

        if (s_announce_pending && (s_announce_force || s_talk_on_change)) {
            s_announce_pending = false;
            s_announce_force   = false;
            send_broadcast_reply(sock, reply);
        }

        if (len < 12) continue;                 /* ArtPoll is the shortest packet */
        if (memcmp(pkt.id, ARTNET_ID, 8) != 0) continue;

        if (pkt.opcode == ARTNET_OP_POLL) {
            if (len >= 13 && (((const uint8_t *)&pkt)[12] & ARTPOLL_TALK_ON_CHANGE))
                s_talk_on_change = true;
            /* Answer to the poller's address and port (controllers that poll
               from port 6454 get the reply on 6454, as the spec requires). */
            size_t n = artnet_build_poll_reply(reply, s_universe);
            sendto(sock, reply, n, 0, (struct sockaddr *)&src, src_len);
            continue;
        }

        if (pkt.opcode == ARTNET_OP_ADDRESS) {
            if (len >= ARTADDRESS_LEN) {
                handle_art_address((const uint8_t *)&pkt);
                size_t n = artnet_build_poll_reply(reply, s_universe);
                sendto(sock, reply, n, 0, (struct sockaddr *)&src, src_len);
            }
            continue;
        }

        if (pkt.opcode == ARTNET_OP_IPPROG) {
            static uint8_t ip_reply[ARTNET_IPPROG_REPLY_LEN];
            bool restart;
            size_t n = artnet_ip_prog((const uint8_t *)&pkt, len, ip_reply, &restart);
            sendto(sock, ip_reply, n, 0, (struct sockaddr *)&src, src_len);
            if (restart) xTaskCreate(restart_task, "restart", 1024, NULL, 1, NULL);
            continue;
        }

        if (pkt.opcode != ARTNET_OP_DMX || len < 18) continue;
        if (pkt.universe != s_universe) continue; /* LE on LE machine */

        uint16_t data_len = ntohs(pkt.length);
        if (data_len > DMX_CHANNELS) data_len = DMX_CHANNELS;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(s_buf, pkt.data, data_len);
        xSemaphoreGive(s_mutex);
        led_artnet_pulse();
        s_pkt_count++;
    }
}

void artnet_start(uint16_t universe)
{
    s_universe = universe;
    s_mutex    = xSemaphoreCreateMutex();
    memset(s_buf, 0, DMX_CHANNELS);
    xTaskCreate(artnet_task, "artnet", 4096, NULL, 5, NULL);
}

uint32_t artnet_get_pkt_count(void) { return s_pkt_count; }

void artnet_set_universe(uint16_t universe)
{
    if (universe != s_universe) {
        s_universe = universe;
        artnet_announce();
    }
}

void artnet_copy_dmx(uint8_t *dst, uint16_t max_len)
{
    uint16_t n = max_len < DMX_CHANNELS ? max_len : DMX_CHANNELS;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(dst, s_buf, n);
    xSemaphoreGive(s_mutex);
}
