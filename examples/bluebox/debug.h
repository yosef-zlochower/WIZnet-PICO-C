#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdint.h>

// Periodic uptime report interval (ms). Change this to adjust frequency.
#define DEBUG_UPTIME_INTERVAL_MS (10 * 60 * 1000)  // 10 minutes

// W5500 socket number for debug (temperature uses socket 0)
#define DEBUG_SOCKET_NUM 1

// Initialize the debug subsystem: opens the debug UDP socket.
// Call after network_setup() and network_open_socket().
// debug_port is typically dest_port_global + 1.
void debug_init(uint16_t debug_port);

// Format and send a debug packet with the given state label.
// state: e.g. "normal", "entering_error", "leaving_error", etc.
void debug_send(const char *state);

// Call from the main loop on each iteration.
// Drains any pending state-change events from led_state and sends debug
// packets for them. Also sends a periodic uptime packet if the interval
// has elapsed.
void debug_poll(void);

#endif
