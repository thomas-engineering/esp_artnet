#include "artnet_ip.h"
#include "artnet.h"
#include "provisioning.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

static const char *TAG = "artnet_ip";

/* ArtIpProg Command bits */
#define IPPROG_ENABLE      0x80   /* any programming needs this bit            */
#define IPPROG_DHCP        0x40   /* enable DHCP, ignore the other bits         */
#define IPPROG_GATEWAY     0x20   /* program default gateway                    */
#define IPPROG_DEFAULTS    0x10   /* all three back to default (= DHCP here)    */
#define IPPROG_IP          0x08   /* program IP address                         */
#define IPPROG_MASK        0x04   /* program subnet mask                        */

/* Packet field offsets (Art-Net 4) */
#define OFF_COMMAND   14
#define OFF_IP        16
#define OFF_MASK      20
#define OFF_GATEWAY   26
#define MIN_LEN       24          /* up to and including the subnet mask */

/* IPv4 addresses are handled as 4 bytes, first octet first (= network order) */
typedef struct { uint8_t ip[4], mask[4], gw[4]; } addr_set_t;

static void to_str(const uint8_t a[4], char *dst, size_t n)
{
    snprintf(dst, n, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

static bool from_str(const char *s, uint8_t a[4])
{
    if (!s || !s[0]) return false;
    uint32_t v = inet_addr(s);
    if (v == IPADDR_NONE) return false;
    memcpy(a, &v, 4);
    return true;
}

/* Address set currently in use by the STA interface */
static void runtime_addrs(addr_set_t *r)
{
    memset(r, 0, sizeof(*r));
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        memcpy(r->ip,   &info.ip.addr,      4);
        memcpy(r->mask, &info.netmask.addr, 4);
        memcpy(r->gw,   &info.gw.addr,      4);
    }
}

/* A valid mask is a run of 1 bits followed by 0 bits, and not all zero */
static bool mask_valid(const uint8_t m[4])
{
    uint32_t v = ((uint32_t)m[0] << 24) | (m[1] << 16) | (m[2] << 8) | m[3];
    uint32_t inv = ~v;
    return v != 0 && (inv & (inv + 1)) == 0;
}

static bool is_zero4(const uint8_t a[4]) { return !(a[0] | a[1] | a[2] | a[3]); }

size_t artnet_ip_prog(const uint8_t *p, int len, uint8_t *out, bool *restart)
{
    *restart = false;

    net_config_t cfg;
    provisioning_get_net_config(&cfg);
    const net_config_t old = cfg;

    uint8_t cmd = (len >= MIN_LEN) ? p[OFF_COMMAND] : 0;

    if (cmd & IPPROG_ENABLE) {
        if (cmd & (IPPROG_DHCP | IPPROG_DEFAULTS)) {
            cfg.dhcp = true;
        } else if (cmd & (IPPROG_IP | IPPROG_MASK | IPPROG_GATEWAY)) {
            /* Start from the static settings, or from what DHCP gave us, and
               overwrite only the parts the controller wants to change. */
            addr_set_t a;
            if (cfg.dhcp || !from_str(cfg.ip, a.ip) ||
                !from_str(cfg.netmask, a.mask) || !from_str(cfg.gateway, a.gw))
                runtime_addrs(&a);
            if (cmd & IPPROG_IP)   memcpy(a.ip,   p + OFF_IP,   4);
            if (cmd & IPPROG_MASK) memcpy(a.mask, p + OFF_MASK, 4);
            if ((cmd & IPPROG_GATEWAY) && len >= OFF_GATEWAY + 4)
                memcpy(a.gw, p + OFF_GATEWAY, 4);

            if (is_zero4(a.ip) || a.ip[0] >= 224 || !mask_valid(a.mask)) {
                ESP_LOGW(TAG, "ArtIpProg: rejected invalid address/mask");
            } else {
                cfg.dhcp = false;
                to_str(a.ip,   cfg.ip,      sizeof(cfg.ip));
                to_str(a.mask, cfg.netmask, sizeof(cfg.netmask));
                to_str(a.gw,   cfg.gateway, sizeof(cfg.gateway));
                if (!cfg.dns[0])        /* keep an existing DNS, else use the gateway */
                    to_str(is_zero4(a.gw) ? (const uint8_t[]){8, 8, 8, 8} : a.gw,
                           cfg.dns, sizeof(cfg.dns));
            }
        }

        bool changed = cfg.dhcp != old.dhcp ||
                       (!cfg.dhcp && (strcmp(cfg.ip, old.ip) || strcmp(cfg.netmask, old.netmask) ||
                                      strcmp(cfg.gateway, old.gateway)));
        if (changed) {
            provisioning_save_net_config(&cfg);
            *restart = true;
            ESP_LOGI(TAG, "ArtIpProg: %s - restarting to apply",
                     cfg.dhcp ? "DHCP enabled" : cfg.ip);
        }
    }

    /* ArtIpProgReply: the settings that are (or, after restart, will be) in use */
    addr_set_t eff;
    if (cfg.dhcp || !from_str(cfg.ip, eff.ip) || !from_str(cfg.netmask, eff.mask) ||
        !from_str(cfg.gateway, eff.gw))
        runtime_addrs(&eff);

    memset(out, 0, ARTNET_IPPROG_REPLY_LEN);
    memcpy(out, "Art-Net\0", 8);
    out[8] = 0x00; out[9] = 0xF9;               /* OpCode 0xF900 (LE) */
    out[11] = 14;                               /* ProtVer lo */
    memcpy(out + 16, eff.ip,   4);              /* ProgIp  */
    memcpy(out + 20, eff.mask, 4);              /* ProgSm  */
    out[24] = ARTNET_PORT >> 8;                 /* ProgPort (deprecated, BE) */
    out[25] = ARTNET_PORT & 0xFF;
    out[26] = cfg.dhcp ? 0x40 : 0x00;           /* Status: bit 6 = DHCP enabled */
    memcpy(out + 28, eff.gw,   4);              /* ProgDg (default gateway) */
    return ARTNET_IPPROG_REPLY_LEN;
}
