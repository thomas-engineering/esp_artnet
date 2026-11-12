#pragma once

/*
 * Debug-only DMX bus sniffer (enabled with -DDMX_DEBUG_SNIFF=1).
 *
 * The transceiver is put into receive mode (DE/RE low) and the DMX frames of
 * another master (e.g. the Arduino UNO) are analysed: BREAK, MAB, frame
 * period, frame duration and slot values. A summary is logged every 2 s.
 */
#ifdef DMX_DEBUG_SNIFF
void dmx_sniff_start(void);
#endif
