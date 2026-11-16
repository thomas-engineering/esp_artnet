#include "sacn.h"
#include "sacn_core.h"
#include "artnet.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lwip/sockets.h"

static const char *TAG = "sacn";

#define NVS_NAMESPACE  "artnet"             /* wiped by the factory reset */
#define NVS_KEY_MODE   "input_mode"
#define NVS_KEY_UNIV   "sacn_univ"

static volatile input_mode_t s_mode = INPUT_ARTNET;
static volatile uint16_t     s_univ_cfg = 0;       /* 0 = automatic */
static volatile uint32_t     s_ip_gen   = 0;       /* bumped on every new IP address */
static sacn_merge_t          s_merge;
static volatile int          s_sources  = 0;

static void nvs_put_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

input_mode_t sacn_get_mode(void) { return s_mode; }

void sacn_set_mode(input_mode_t mode)
{
    if (mode != INPUT_ARTNET && mode != INPUT_SACN) return;
    if (mode == s_mode) return;
    s_mode = mode;
    nvs_put_u8(NVS_KEY_MODE, (uint8_t)mode);
    ESP_LOGI(TAG, "Input protocol: %s", mode == INPUT_SACN ? "sACN (E1.31)" : "Art-Net");
    artnet_announce();
}

uint16_t sacn_get_universe(void)
{
    unsigned u = s_univ_cfg ? s_univ_cfg : (unsigned)artnet_get_universe() + 1;
    return sacn_universe_valid(u) ? (uint16_t)u : 1;
}

void sacn_set_universe(uint16_t universe)
{
    if (universe != 0 && !sacn_universe_valid(universe)) return;
    if (universe == s_univ_cfg) return;
    s_univ_cfg = universe;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY_UNIV, universe);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "sACN universe: %u%s", sacn_get_universe(), universe ? "" : " (automatic)");
}

int sacn_get_sources(void) { return s_sources; }

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_ip_gen++;                             /* the multicast group is joined afresh */
}

static uint32_t sta_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return 0;
    return info.ip.addr;
}

static int group_op(int sock, int opt, uint16_t universe, uint32_t ifaddr)
{
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = htonl(sacn_multicast_addr(universe)),
        .imr_interface.s_addr = ifaddr,
    };
    return setsockopt(sock, IPPROTO_IP, opt, &mreq, sizeof(mreq));
}

static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(SACN_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

/* The socket exists only while sACN is the selected input. Unicast to port 5568
   is received without any group; the multicast group of the universe is joined
   as soon as the node has an IP address, and again after every reconnect. */
static void sacn_task(void *arg)
{
    static uint8_t buf[SACN_MAX_LEN + 64];
    int      sock = -1;
    bool     joined = false;
    uint16_t joined_univ = 0;
    uint32_t joined_if = 0, joined_gen = 0;

    while (1) {
        if (s_mode != INPUT_SACN) {
            if (sock >= 0) {
                close(sock);                /* also drops the group membership */
                sock = -1;
                joined = false;
                memset(&s_merge, 0, sizeof(s_merge));
                s_sources = 0;
                ESP_LOGI(TAG, "sACN receiver stopped");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (sock < 0) {
            sock = open_socket();
            if (sock < 0) {
                ESP_LOGE(TAG, "socket/bind failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "Listening for sACN on UDP port %d", SACN_PORT);
        }

        uint16_t univ = sacn_get_universe();
        uint32_t gen  = s_ip_gen;
        uint32_t ip   = sta_ip();
        if (joined && (univ != joined_univ || gen != joined_gen)) {
            group_op(sock, IP_DROP_MEMBERSHIP, joined_univ, joined_if);
            joined = false;
            memset(&s_merge, 0, sizeof(s_merge));       /* other universe: no old sources */
        }
        if (!joined && ip != 0) {
            if (group_op(sock, IP_ADD_MEMBERSHIP, univ, ip) == 0) {
                joined = true;
                joined_univ = univ;
                joined_if   = ip;
                joined_gen  = gen;
                ESP_LOGI(TAG, "Joined multicast group 239.255.%u.%u (universe %u)",
                         univ >> 8, univ & 0xFF, univ);
            } else {
                ESP_LOGW(TAG, "Joining the multicast group failed (unicast still works)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        int len = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        int64_t now = esp_timer_get_time();
        s_sources = sacn_core_active(&s_merge, now);
        if (len < SACN_MIN_LEN) continue;

        uint8_t *out = artnet_output_lock();
        sacn_rx_t r = sacn_core_dmx(&s_merge, buf, len, now, univ, artnet_merge_ltp(), out);
        artnet_output_unlock(r == SACN_RX_OUTPUT, s_merge.merging);
        if (r != SACN_RX_IGNORED) s_sources = sacn_core_active(&s_merge, now);
    }
}

void sacn_start(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t  mode = INPUT_ARTNET;
        uint16_t univ = 0;
        nvs_get_u8(h, NVS_KEY_MODE, &mode);
        nvs_get_u16(h, NVS_KEY_UNIV, &univ);
        nvs_close(h);
        s_mode     = mode == INPUT_SACN ? INPUT_SACN : INPUT_ARTNET;
        s_univ_cfg = (univ == 0 || sacn_universe_valid(univ)) ? univ : 0;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    xTaskCreate(sacn_task, "sacn", 4096, NULL, 5, NULL);
}
