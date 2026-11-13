#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTNET_POLL_REPLY_LEN 239   /* fixed size of an ArtPollReply (protocol 14) */

#define ARTNET_SHORT_NAME_LEN 18    /* incl. terminator: 17 characters */
#define ARTNET_LONG_NAME_LEN  64    /* incl. terminator: 63 characters */

/* Node names as shown in ArtPollReply, stored in NVS.
   For both functions below: NULL leaves a name unchanged; non-ASCII and control
   characters are replaced by '?'. Return true if a name actually changed. */

/* ArtAddress semantics: an empty string also means "no change". */
bool artnet_names_set(const char *short_name, const char *long_name);

/* Web UI semantics: an empty string resets the name to its default. */
bool artnet_names_update(const char *short_name, const char *long_name);

/* Currently effective names (custom or default). */
void artnet_names_get(char *short_out, size_t short_sz,
                      char *long_out, size_t long_sz);

/* Loads the stored names; call once before other tasks use this module. */
void artnet_poll_init(void);

/* Builds an ArtPollReply describing this node (one DMX output port on the
   configured universe). buf must hold ARTNET_POLL_REPLY_LEN bytes.
   Returns the packet length. */
size_t artnet_build_poll_reply(uint8_t *buf, uint16_t universe);
