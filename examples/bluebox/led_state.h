#ifndef _LED_STATE_H_
#define _LED_STATE_H_

#include <stdbool.h>

void enter_error_state(void);
void leave_error_state(void);
void enter_config_state(void);
void leave_config_state(void);

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

#endif
