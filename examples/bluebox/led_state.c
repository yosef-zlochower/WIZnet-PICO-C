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
}
