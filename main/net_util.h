#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Strict dotted-quad parser ("a.b.c.d" only, no shorthand forms, no trailing
   characters). Returns false on any other input. Bytes are in network order. */
bool net_parse_ipv4(const char *s, uint8_t out[4]);

/* A valid mask is a run of 1 bits followed by 0 bits, not all zero.
   /32 is rejected as well (no host bits). */
bool net_mask_valid(const uint8_t mask[4]);

/* Usable unicast host address: not 0.x, 127.x, multicast/reserved (>= 224) */
bool net_host_ip_valid(const uint8_t ip[4]);

/* Complete static configuration: ip, mask, gateway and dns as strings.
   Checks syntax, mask, that ip is a host address inside its subnet (not the
   network or broadcast address), and that the gateway is a different host
   address in the same subnet. DNS must be a valid host address.
   On failure *reason points to a static message. */
bool net_static_config_valid(const char *ip, const char *mask, const char *gw,
                             const char *dns, const char **reason);
