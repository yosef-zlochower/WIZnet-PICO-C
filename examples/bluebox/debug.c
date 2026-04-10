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
