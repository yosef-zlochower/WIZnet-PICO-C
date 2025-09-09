/*
 * ds18b20.c
 * This file contains the functions for the one-wire communication
 * and reading temperature from the DS18B20 sensor.
 */
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <pico/multicore.h>
#include <stdio.h>
#include <string.h>

#include "ds18b20.h"
#include "hardware.h"
#include "globals.h"
#include "led_state.h"



// --- One-Wire and DS18B20 Protocol ---
#define DS18B20_CONVERT_T 0x44
#define DS18B20_READ_SCRATCHPAD 0xbe
#define DS18B20_ERROR -128

static bool one_wire_reset() {
  gpio_set_dir(ONE_WIRE_PIN, GPIO_OUT);
  gpio_put(ONE_WIRE_PIN, 0);
  sleep_us(480);
  gpio_set_dir(ONE_WIRE_PIN, GPIO_IN);
  sleep_us(70);
  bool presence = !gpio_get(ONE_WIRE_PIN);
  sleep_us(410);
  return presence;
}

static bool check_for_ds18b20(void) {
  // Returns true if sensor found right away
  // Returns false if sensor has disconnected temporarily
  // never returns if the sensor is never found
  bool found = one_wire_reset();
  if (found) {
    return true;
  }

  enter_error_state();
  while (!one_wire_reset()) {
    sleep_us(5000000);
  };
  leave_error_state();
  return false;
}
static void one_wire_write_bit(bool bit) {
  gpio_set_dir(ONE_WIRE_PIN, GPIO_OUT);
  if (bit) {
    gpio_put(ONE_WIRE_PIN, 0);
    sleep_us(6);
    gpio_set_dir(ONE_WIRE_PIN, GPIO_IN);
    sleep_us(64);
  } else {
    gpio_put(ONE_WIRE_PIN, 0);
    sleep_us(60);
    gpio_set_dir(ONE_WIRE_PIN, GPIO_IN);
    sleep_us(10);
  }
}

static bool one_wire_read_bit() {
  bool result = 0;
  gpio_set_dir(ONE_WIRE_PIN, GPIO_OUT);
  gpio_put(ONE_WIRE_PIN, 0);
  sleep_us(6);
  gpio_set_dir(ONE_WIRE_PIN, GPIO_IN);
  sleep_us(9);
  result = gpio_get(ONE_WIRE_PIN);
  sleep_us(55);
  return result;
}

static void one_wire_write_byte(uint8_t byte) {
  for (int i = 0; i < 8; i++) {
    one_wire_write_bit((byte >> i) & 1);
  }
}

static uint8_t one_wire_read_byte() {
  uint8_t byte = 0;
  for (int i = 0; i < 8; i++) {
    if (one_wire_read_bit()) {
      byte |= (1 << i);
    }
  }
  return byte;
}

static int ds18b20_read_temp() {
  // We need to return error value if sensor was disconnected during this
  // function call. If the sensor is reconnected the temperature recorded will
  // be wrong.
  uint8_t scratchpad[9];
  if (!check_for_ds18b20()) {
    return DS18B20_ERROR;
  }
  one_wire_write_byte(0xcc);
  one_wire_write_byte(DS18B20_CONVERT_T);
  sleep_ms(750);
  if (!check_for_ds18b20()) {
    return DS18B20_ERROR;
  }
  one_wire_write_byte(0xcc);
  one_wire_write_byte(DS18B20_READ_SCRATCHPAD);
  for (int i = 0; i < 9; i++) {
    scratchpad[i] = one_wire_read_byte();
  }
  int16_t raw_temp = (scratchpad[1] << 8) | scratchpad[0];
  float temp_c = (float)raw_temp / 16.0f;
  int temp_f = (int)(temp_c * 1.8f + 32.0f + 0.5f);
  if (temp_f < 0 || temp_c > 212) {
    return DS18B20_ERROR;
  }
  return temp_f;
}

// --- Core 1 Entry ---
void ds18b20_core1_entry() {
#define APPROX_TIME_FOR_ONE_READ_ms 759
  if (time_delay_global < 1)
  {
    time_delay_global = 1;
  }
  int delay = time_delay_global * 1000 - APPROX_TIME_FOR_ONE_READ_ms;
  for (;;) {
    gpio_put(LED_PIN, 0); // Turn off LED
    int temp_f = ds18b20_read_temp();
    if (temp_f != DS18B20_ERROR && temp_f >= 0 && temp_f <= 255) {
      memcpy(packet_buffer, packet_pattern, packet_size);
      packet_buffer[temperature_byte_index] = (uint8_t)temp_f;
      multicore_fifo_push_blocking(1);
    } else {
      printf("CRITICAL: Failed to get valid temp.\n");
    }
    gpio_put(LED_PIN, 1); // Turn on LED
    sleep_ms(delay);
  }
}

void initialize_ds18b20(void) {
  gpio_init(ONE_WIRE_PIN);
  check_for_ds18b20();
}
