#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTNET_IPPROG_REPLY_LEN 34   /* fixed size of an ArtIpProgReply */

/*
 * Handles an ArtIpProg packet (remote IP configuration).
 *
 * pkt/len is the received packet (opcode 0xF800). On a programming command the
 * new network configuration is stored in NVS (same storage as the web UI), and
 * *restart is set because the settings only take effect after a reboot.
 * A command without the "enable programming" bit is a pure query.
 *
 * The ArtIpProgReply is written to out (ARTNET_IPPROG_REPLY_LEN bytes); the
 * return value is its length. The caller sends it and restarts if requested.
 */
size_t artnet_ip_prog(const uint8_t *pkt, int len, uint8_t *out, bool *restart);
