# Debug UDP Feature - Implementation Plan

## Problem

When tapping the bluebox device, the USB serial connection to the host
computer resets. We need to determine if the entire device is resetting
(power/hardware fault) or if only the USB interface is affected. Since the
device normally runs without a USB connection, we need a non-USB diagnostic
channel.

## Solution

Send debug information as plain-text UDP packets to the same destination
host on `dest_port + 1`. A host-side listener (e.g., `nc -lu 16217`) can
log these packets. If the device reboots, the uptime in the packets will
reset to `00:00:00`, making a full reset immediately visible.

## Packet Format

```
DEBUG: <state> HH:MM:SS\n
```

Examples:
```
DEBUG: normal 01:23:45
DEBUG: entering_error 00:05:12
DEBUG: leaving_error 00:05:17
DEBUG: entering_config 00:00:03
DEBUG: leaving_config 00:01:40
```

Maximum packet length: `"DEBUG: entering_config HHHHHHH:MM:SS\n"` = 38 bytes.
The hours field is 7 digits wide, supporting uptimes beyond 1000 years.

## When Debug Packets Are Sent

1. **Periodic uptime** -- every 10 minutes (tunable via `#define`), state
   reported as `normal` (or whatever the current state is).
2. **State transitions** -- each time the LED state machine enters or leaves
   ERROR or CONFIG state.

## Design Decisions

### Socket allocation

The W5500 provides 8 independent hardware sockets. Temperature data uses
socket 0 (`SOCKET_NUM`). Debug will use socket 1 (`DEBUG_SOCKET_NUM`).
Both sockets send to the same destination IP.

### All network sends on core 0

The W5500's SPI bus is shared. To avoid contention between cores, all
`sendto()` calls (temperature and debug) happen on core 0. State
transitions triggered by core 1 (sensor disconnect in `ds18b20.c`) are
communicated to core 0 via a volatile flag rather than sending directly.

### Main loop change: blocking pop -> timeout pop

Currently core 0 blocks indefinitely on `multicore_fifo_pop_blocking()`.
This will change to `multicore_fifo_pop_timeout_us()` with a 1-second
timeout so that core 0 can:
- Detect state changes flagged by core 1
- Send periodic uptime packets on schedule

### State change detection

`led_state.c` already manages the state machine. It will be extended to:
- Expose the current state via a getter
- Record state-change events in a small ring buffer (up to 4 entries)

Core 0 drains this ring buffer on each loop iteration and sends a debug
packet for each recorded transition. A ring buffer (rather than a single
flag) avoids losing rapid enter/leave transitions that happen between
core 0 loop iterations.

### Uptime source

`time_us_64()` from the Pico SDK provides microseconds since power-on as
a `uint64_t`, which will not wrap for over 584,000 years. This is divided
down to seconds and converted to `HH:MM:SS` for display. The hours field
is not capped at 24 and supports up to 7 digits (i.e., uptime of 3 days
shows as `72:00:00`).

### Pre-network state transitions

`network_setup()` calls `enter_error_state()` / `leave_error_state()` during
WIZnet initialization, before the debug socket exists. These transitions will
be silently dropped (the ring buffer will be drained and discarded before the
debug socket opens). This is expected -- you can't send UDP before the
network is up.

---

## Files to Create

### `examples/bluebox/debug.h`

```c
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
```

### `examples/bluebox/debug.c`

```c
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "socket.h"

#include "debug.h"
#include "globals.h"
#include "led_state.h"

static uint8_t debug_dest_ip[4];
static uint16_t debug_dest_port;
static uint64_t last_periodic_send_us;
static bool debug_initialized = false;

void debug_init(uint16_t debug_port)
{
    memcpy(debug_dest_ip, dest_ip_global, 4);
    debug_dest_port = debug_port;

    int result = socket(DEBUG_SOCKET_NUM, Sn_MR_UDP, debug_port, 0);
    if (result == DEBUG_SOCKET_NUM)
    {
        printf("Debug UDP socket opened on port %d.\n", debug_port);
        debug_initialized = true;
    }
    else
    {
        printf("Failed to open debug socket. Error: %d\n", result);
    }

    last_periodic_send_us = time_us_64();

    // Drain any state-change events that occurred before network was ready
    while (state_event_pop(NULL)) {}

    // Send initial boot message
    debug_send("boot");
}

void debug_send(const char *state)
{
    if (!debug_initialized)
        return;

    uint64_t total_secs = time_us_64() / 1000000;
    uint64_t hours = total_secs / 3600;
    uint32_t minutes = (uint32_t)((total_secs % 3600) / 60);
    uint32_t seconds = (uint32_t)(total_secs % 60);

    char buf[48];
    int len = snprintf(buf, sizeof(buf),
                       "DEBUG: %s %llu:%02lu:%02lu\n",
                       state,
                       (unsigned long long)hours,
                       (unsigned long)minutes,
                       (unsigned long)seconds);

    sendto(DEBUG_SOCKET_NUM, (uint8_t *)buf, len,
           debug_dest_ip, debug_dest_port);
}

void debug_poll(void)
{
    if (!debug_initialized)
        return;

    // Drain state-change events
    state_event_t event;
    while (state_event_pop(&event))
    {
        static const char *event_names[] = {
            [EVT_ENTERING_ERROR]  = "entering_error",
            [EVT_LEAVING_ERROR]   = "leaving_error",
            [EVT_ENTERING_CONFIG] = "entering_config",
            [EVT_LEAVING_CONFIG]  = "leaving_config",
        };
        if (event < sizeof(event_names) / sizeof(event_names[0])
            && event_names[event] != NULL)
        {
            debug_send(event_names[event]);
        }
    }

    // Periodic uptime
    uint64_t now_us = time_us_64();
    if ((now_us - last_periodic_send_us) >= (uint64_t)DEBUG_UPTIME_INTERVAL_MS * 1000)
    {
        last_periodic_send_us = now_us;
        const char *state_name = get_state_name();
        debug_send(state_name);
    }
}
```

---

## Files to Modify

### 1. `examples/bluebox/led_state.h`

Add state query API and event ring buffer interface:

```c
// --- existing declarations remain ---
void enter_error_state(void);
void leave_error_state(void);
void enter_config_state(void);
void leave_config_state(void);

// --- new declarations ---
typedef enum {
    EVT_ENTERING_ERROR = 0,
    EVT_LEAVING_ERROR,
    EVT_ENTERING_CONFIG,
    EVT_LEAVING_CONFIG,
} state_event_t;

// Returns the name of the current state: "normal", "error", or "config".
const char *get_state_name(void);

// Pop one event from the ring buffer. Returns true if an event was
// available.  If out is non-NULL, the event type is written to *out.
bool state_event_pop(state_event_t *out);
```

### 2. `examples/bluebox/led_state.c`

Add a small ring buffer (capacity 4) and push an event in each
enter/leave function. Add `get_state_name()` and `state_event_pop()`.

Specifically:

- After the existing `enum state_t` / `current_state` declarations, add:

```c
#include "led_state.h"   // for state_event_t

#define EVENT_RING_SIZE 8
static volatile state_event_t event_ring[EVENT_RING_SIZE];
static volatile uint8_t event_ring_head = 0;  // next write position
static volatile uint8_t event_ring_tail = 0;  // next read position

static void event_push(state_event_t evt)
{
    uint8_t next = (event_ring_head + 1) % EVENT_RING_SIZE;
    if (next != event_ring_tail)  // drop if full
    {
        event_ring[event_ring_head] = evt;
        event_ring_head = next;
    }
}
```

- In `enter_error_state()`, after setting `current_state = ERROR`, add:
  `event_push(EVT_ENTERING_ERROR);`

- In `leave_error_state()`, after setting `current_state = NORMAL`, add:
  `event_push(EVT_LEAVING_ERROR);`

- In `enter_config_state()`, after setting `current_state = CONFIG`, add:
  `event_push(EVT_ENTERING_CONFIG);`

- In `leave_config_state()`, after setting `current_state = NORMAL`, add:
  `event_push(EVT_LEAVING_CONFIG);`

- Add the two new public functions:

```c
const char *get_state_name(void)
{
    switch (current_state)
    {
        case ERROR:  return "error";
        case CONFIG: return "config";
        default:     return "normal";
    }
}

bool state_event_pop(state_event_t *out)
{
    if (event_ring_tail == event_ring_head)
        return false;
    state_event_t evt = event_ring[event_ring_tail];
    event_ring_tail = (event_ring_tail + 1) % EVENT_RING_SIZE;
    if (out)
        *out = evt;
    return true;
}
```

### 3. `examples/bluebox/main.c`

Three changes:

**a) Add include:**
```c
#include "debug.h"
```

**b) After `network_open_socket()`, add debug init:**
```c
network_open_socket(dest_port_global);
debug_init(dest_port_global + 1);          // <-- new
```

**c) Replace the main loop** -- change from blocking FIFO pop to
timeout-based, and add `debug_poll()`:

```c
for (;;)
{
    uint32_t value;
    bool got_data = multicore_fifo_pop_timeout_us(1000000, &value);

    if (got_data && value == 1)
    {
        printf("The temperature is %u F\n",
               packet_buffer[temperature_byte_index]);

        int32_t len = network_send_packet(packet_buffer, packet_size,
                                          dest_ip_global, dest_port_global);
        if (len > 0)
        {
            printf("Sent UDP packet to %d.%d.%d.%d:%d\n",
                   dest_ip_global[0], dest_ip_global[1],
                   dest_ip_global[2], dest_ip_global[3],
                   dest_port_global);
        }
        else
        {
            printf("Failed to send UDP packet. Error: %ld\n", len);
        }
    }

    debug_poll();
}
```

### 4. `examples/bluebox/CMakeLists.txt`

Add `debug.c` to the source list:

```cmake
add_executable(${TARGET_NAME}
    main.c
    config.c
    ds18b20.c
    network.c
    led_state.c
    debug.c            # <-- new
)
```

### 5. `examples/bluebox/globals.h`

No changes needed. `debug.c` accesses `dest_ip_global` and
`dest_port_global` which are already declared extern here.

---

## Unchanged Files

| File | Why unchanged |
|------|---------------|
| `config.h` / `config.c` | Debug port is derived (`dest_port + 1`), not stored in flash config |
| `ds18b20.h` / `ds18b20.c` | No changes; state events are captured in `led_state.c` which ds18b20 already calls |
| `network.h` / `network.c` | Debug socket uses the WIZnet socket API directly; no new network.c functions needed |
| `hardware.h` | No new pins |
| Top-level `CMakeLists.txt` | No new libraries needed |

---

## Thread Safety Analysis

The ring buffer in `led_state.c` is the only data structure shared between
cores. It is a single-producer-single-consumer (SPSC) design:

- **Writes (event_push)**: called from `enter_*` / `leave_*` functions.
  During normal operation after core 1 launches, these are only called from
  core 1 (via `check_for_ds18b20`). The pre-launch calls from core 0
  (during `network_setup`) happen before core 1 exists, so there is no
  concurrent access.
- **Reads (state_event_pop)**: called only from `debug_poll()` on core 0.

With one writer and one reader, and `head`/`tail` being single-byte
atomically-updated indices, the ring buffer is safe without locks. The
`volatile` qualifier ensures visibility across cores.

`get_state_name()` reads `current_state`, which is written by the
enter/leave functions. This is a single enum read used for display
purposes only, so a stale value is harmless.

---

## Testing

### On the host machine

Listen for debug packets:

```bash
nc -lu 16217
```

(Assuming dest_port is 16216, debug port will be 16217.)

### Expected output after boot

```
DEBUG: boot 0:00:05
```

Then every 10 minutes:

```
DEBUG: normal 0:10:05
DEBUG: normal 0:20:05
```

### Simulating a sensor disconnect

Unplug the DS18B20. Expect:

```
DEBUG: entering_error 0:12:33
```

Reconnect. Expect:

```
DEBUG: leaving_error 0:12:41
```

### Tap test

Tap the device. If uptime keeps counting up, the device is not resetting
and the problem is USB-only. If uptime resets to `0:00:xx` with a new
`boot` message, the device is doing a full reset.

---

## Tunable Constants

| Constant | File | Default | Purpose |
|----------|------|---------|---------|
| `DEBUG_UPTIME_INTERVAL_MS` | `debug.h` | `600000` (10 min) | Periodic uptime report interval |
| `DEBUG_SOCKET_NUM` | `debug.h` | `1` | W5500 socket for debug |
| `EVENT_RING_SIZE` | `led_state.c` | `8` | Max queued state-change events |
| FIFO timeout | `main.c` | `1000000` (1 sec) | How often core 0 wakes to check debug |

The debug port itself is `dest_port_global + 1`, derived at runtime.
