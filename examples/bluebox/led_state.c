#include <ctype.h>
#include <pico/multicore.h>
#include <pico/stdio_usb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/gpio.h"

#include "config.h"
#include "hardware.h"
#include "led_state.h"


static volatile bool led_on = false;
// This is the callback function that the timer will execute.
static bool repeating_timer_callback(struct repeating_timer *t)
{
    // Toggle the state of the LED pin.
    if (led_on)
    {
        gpio_put(LED_PIN, 0); // Turn LED off
        led_on = false;
    }
    else
    {
        gpio_put(LED_PIN, 1); // Turn LED on
        led_on = true;
    }
    // Return true to continue the timer.
    return true;
}
enum state_t
{ NORMAL=0,
  ERROR=1,
  CONFIG=2
};

static enum state_t current_state = NORMAL;

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

static struct repeating_timer timer;

void enter_error_state(void)
{
    printf("entering ERROR state\n");
    if (current_state!= NORMAL)
    {
      printf("WARNING: Entered ERROR state from non-NORMAL state\n"
             "this should never happen\n");
      cancel_repeating_timer(&timer);
    }
    current_state = ERROR;
    event_push(EVT_ENTERING_ERROR);
    add_repeating_timer_us(-125000, repeating_timer_callback, NULL, &timer);
}

void leave_error_state(void)
{
    printf("leaving ERROR state\n");
    if (current_state!= ERROR)
    {
      printf("WARNING: Attempting to leave ERROR state,\n"
             "but current state is not ERROR.\n"
             "this should never happen\n");
      if (current_state != NORMAL)
      {
        cancel_repeating_timer(&timer);
      }
      enter_error_state();
      do
      {;}while(1);
    }
    cancel_repeating_timer(&timer);
    current_state=NORMAL;
    event_push(EVT_LEAVING_ERROR);
}

void enter_config_state(void)
{
    printf("entering CONFIG state\n");
    if (current_state!= NORMAL)
    {
      printf("WARNING: Entered CONFIG state from non-NORMAL state\n"
             "this should never happen\n");
      cancel_repeating_timer(&timer);
      current_state = NORMAL;
      enter_error_state();
    }
  current_state=CONFIG;
    event_push(EVT_ENTERING_CONFIG);
    add_repeating_timer_us(-250000, repeating_timer_callback, NULL, &timer);
}

void leave_config_state(void)
{
    if (current_state!= CONFIG)
    {
      printf("WARNING: Attempting to leave CONFIG state,\n"
             "but current state is not CONFIG.\n"
             "this should never happen\n");
      if (current_state != NORMAL)
      {
        cancel_repeating_timer(&timer);
      }
      current_state = NORMAL;
      enter_error_state();
      do
      {}while(1);
    }
    printf("leaving CONFIG state\n");
    cancel_repeating_timer(&timer);
    current_state=NORMAL;
    event_push(EVT_LEAVING_CONFIG);
}

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
