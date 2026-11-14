#include "net_util.h"
#include <string.h>
#include "lwip/sockets.h"

bool net_parse_ipv4(const char *s, uint8_t out[4])
{
    if (!s) return false;
    /* Only digits and dots: inet_pton would otherwise be the only gatekeeper */
    size_t n = strlen(s);
    if (n < 7 || n > 15) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) return false;
    return inet_pton(AF_INET, s, out) == 1;
}

static uint32_t be32(const uint8_t a[4])
{
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

bool net_mask_valid(const uint8_t mask[4])
{
    uint32_t v = be32(mask);
    uint32_t inv = ~v;
    return v != 0 && v != 0xFFFFFFFFu && (inv & (inv + 1)) == 0;
}

bool net_host_ip_valid(const uint8_t ip[4])
{
    return ip[0] != 0 && ip[0] != 127 && ip[0] < 224;
}

bool net_static_config_valid(const char *ip_s, const char *mask_s, const char *gw_s,
                             const char *dns_s, const char **reason)
{
    uint8_t ip[4], mask[4], gw[4], dns[4];
    const char *r;
    r = NULL;

    if (!net_parse_ipv4(ip_s, ip))        r = "Invalid IP address";
    else if (!net_parse_ipv4(mask_s, mask)) r = "Invalid subnet mask";
    else if (!net_parse_ipv4(gw_s, gw))   r = "Invalid gateway";
    else if (!net_parse_ipv4(dns_s, dns)) r = "Invalid DNS server";
    else if (!net_mask_valid(mask))       r = "Invalid subnet mask";
    else if (!net_host_ip_valid(ip))      r = "IP address is not a usable host address";
    else {
        uint32_t i = be32(ip), m = be32(mask), g = be32(gw);
        if ((i & ~m) == 0 || (i | m) == 0xFFFFFFFFu) r = "IP address is the network or broadcast address";
        else if (!net_host_ip_valid(gw))          r = "Invalid gateway";
        else if ((g & m) != (i & m))              r = "Gateway is not in the subnet";
        else if (g == i)                          r = "Gateway equals the IP address";
        else if ((g & ~m) == 0 || (g | m) == 0xFFFFFFFFu) r = "Gateway is the network or broadcast address";
        else if (!net_host_ip_valid(dns))         r = "Invalid DNS server";
    }
    if (reason) *reason = r;
    return r == NULL;
}
