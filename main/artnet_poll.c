#include "artnet_poll.h"
#include "artnet.h"
#include "provisioning.h"
#include "led.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "nvs.h"

#define ART_OEM_UNKNOWN   0x00FFu   /* no registered OEM code */
#define ART_ESTA_PROTO    0x7FF0u   /* ESTA manufacturer code for prototypes */

#define NAMES_NVS_NAMESPACE  "artnet"
#define SHORT_NAME_LEN       ARTNET_SHORT_NAME_LEN
#define LONG_NAME_LEN        ARTNET_LONG_NAME_LEN
#define DEFAULT_SHORT_NAME   "ESP Art-Net"

static char s_short_name[SHORT_NAME_LEN];   /* empty = default */
static char s_long_name[LONG_NAME_LEN];
static bool s_names_loaded;

static void names_load(void)
{
    if (s_names_loaded) return;
    s_names_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NAMES_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_short_name);
    if (nvs_get_str(h, "sname", s_short_name, &n) != ESP_OK) s_short_name[0] = '\0';
    n = sizeof(s_long_name);
    if (nvs_get_str(h, "lname", s_long_name, &n) != ESP_OK) s_long_name[0] = '\0';
    nvs_close(h);
}

/* Copies src into dst (size n): ASCII only, anything else becomes '?' */
static void copy_ascii(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[i] = '\0';
}

bool artnet_names_update(const char *short_name, const char *long_name)
{
    names_load();
    bool changed = false;
    char tmp[LONG_NAME_LEN];

    if (short_name) {
        copy_ascii(tmp, SHORT_NAME_LEN, short_name);
        if (strcmp(tmp, s_short_name) != 0) {
            strlcpy(s_short_name, tmp, sizeof(s_short_name));
            changed = true;
        }
    }
    if (long_name) {
        copy_ascii(tmp, LONG_NAME_LEN, long_name);
        if (strcmp(tmp, s_long_name) != 0) {
            strlcpy(s_long_name, tmp, sizeof(s_long_name));
            changed = true;
        }
    }
    if (changed) {
        nvs_handle_t h;
        if (nvs_open(NAMES_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "sname", s_short_name);
            nvs_set_str(h, "lname", s_long_name);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    return changed;
}

bool artnet_names_set(const char *short_name, const char *long_name)
{
    return artnet_names_update(short_name && short_name[0] ? short_name : NULL,
                               long_name  && long_name[0]  ? long_name  : NULL);
}

void artnet_names_get(char *short_out, size_t short_sz,
                      char *long_out, size_t long_sz)
{
    names_load();
    snprintf(short_out, short_sz, "%s",
             s_short_name[0] ? s_short_name : DEFAULT_SHORT_NAME);
    if (s_long_name[0])
        snprintf(long_out, long_sz, "%s", s_long_name);
    else
        snprintf(long_out, long_sz, "ESP32 Art-Net DMX Node v%s",
                 esp_app_get_description()->version);
}

size_t artnet_build_poll_reply(uint8_t *p, uint16_t universe)
{
    memset(p, 0, ARTNET_POLL_REPLY_LEN);
    names_load();

    /* IP address and MAC of the STA interface */
    uint8_t ip[4] = {0};
    uint8_t mac[6] = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK)
            memcpy(ip, &info.ip.addr, 4);       /* already in network order */
        esp_netif_get_mac(netif, mac);
    }

    /* Firmware version "major.minor.patch" -> VersInfo = minor, patch */
    const char *ver = esp_app_get_description()->version;
    unsigned vmaj = 0, vmin = 0, vpat = 0;
    sscanf(ver, "%u.%u.%u", &vmaj, &vmin, &vpat);

    net_config_t net;
    provisioning_get_net_config(&net);

    memcpy(p, "Art-Net\0", 8);                  /* ID */
    p[8]  = 0x00; p[9]  = 0x21;                 /* OpCode 0x2100 ArtPollReply (LE) */
    memcpy(p + 10, ip, 4);                      /* IpAddress */
    p[14] = ARTNET_PORT & 0xFF;                 /* Port (LE) */
    p[15] = ARTNET_PORT >> 8;
    p[16] = (uint8_t)vmin;                      /* VersInfoH */
    p[17] = (uint8_t)vpat;                      /* VersInfoL */
    p[18] = (universe >> 8) & 0x7F;             /* NetSwitch */
    p[19] = (universe >> 4) & 0x0F;             /* SubSwitch */
    p[20] = ART_OEM_UNKNOWN >> 8;               /* OemHi */
    p[21] = ART_OEM_UNKNOWN & 0xFF;             /* Oem */
    /* Status1: bits 7-6 indicator state (01 locate, 10 mute, 11 normal),
       bits 5-4 = 10 (port-address set programmatically) */
    led_mode_t lm = led_get_mode();
    p[23] = (lm == LED_MODE_LOCATE ? 0x40 : lm == LED_MODE_MUTE ? 0x80 : 0xC0) | 0x20;
    p[24] = ART_ESTA_PROTO & 0xFF;              /* EstaManLo */
    p[25] = ART_ESTA_PROTO >> 8;                /* EstaManHi */
    /* PortName / LongName: user-defined (ArtAddress or web UI), else defaults */
    char sname[SHORT_NAME_LEN] = {0}, lname[LONG_NAME_LEN] = {0};   /* zero padding */
    artnet_names_get(sname, sizeof(sname), lname, sizeof(lname));
    memcpy(p + 26, sname, sizeof(sname));
    memcpy(p + 44, lname, sizeof(lname));
    snprintf((char *)p + 108, 64, "#0001 [%04u] OK",
             (unsigned)(artnet_get_pkt_count() % 10000u));  /* NodeReport */
    p[173] = 1;                                 /* NumPortsLo: one port */
    p[174] = 0x80;                              /* PortTypes[0]: DMX512 output */
    p[182] = 0x80;                              /* GoodOutput[0]: data transmitted */
    p[190] = universe & 0x0F;                   /* SwOut[0] */
    p[200] = 0x00;                              /* Style: StNode */
    memcpy(p + 201, mac, 6);                    /* MAC */
    memcpy(p + 207, ip, 4);                     /* BindIp */
    p[211] = 1;                                 /* BindIndex */
    p[212] = 0x01 | 0x04 | 0x08                 /* web config, DHCP capable, 15-bit */
           | (net.dhcp ? 0x02 : 0);             /* IP set by DHCP */

    return ARTNET_POLL_REPLY_LEN;
}
