/*
 * ds18b20.c
 * This file contains the functions for the one-wire communication
 * and reading temperature from the DS18B20 sensor.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <pico/multicore.h>

#include "ds18b20.h"
#include "hardware.h"


// --- Packet Buffer ---
#define PACKET_SIZE 17
#define TEMPERATURE_BYTE_INDEX 16
static const uint8_t UUID_PATTERN[16] = {
    0xd4, 0x3f, 0x9b, 0x60, 0xec, 0x14, 0x54, 0xd6,
    0x95, 0xc9, 0x3f, 0x3a, 0x4b, 0x81, 0x6e, 0xaa
};
extern uint8_t packet_buffer[PACKET_SIZE]; // Access the buffer from main.c

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
    uint8_t scratchpad[9];
    if (!one_wire_reset())
    {
       return DS18B20_ERROR;
    }
    one_wire_write_byte(0xcc);
    one_wire_write_byte(DS18B20_CONVERT_T);
    sleep_ms(750);
    if (!one_wire_reset()) return DS18B20_ERROR;
    one_wire_write_byte(0xcc);
    one_wire_write_byte(DS18B20_READ_SCRATCHPAD);
    for (int i = 0; i < 9; i++) {
        scratchpad[i] = one_wire_read_byte();
    }
    int16_t raw_temp = (scratchpad[1] << 8) | scratchpad[0];
    float temp_c = (float)raw_temp / 16.0f;
    int temp_f = (int)(temp_c * 1.8f + 32.0f + 0.5f);
    if (temp_c < -55.0f || temp_c > 125.0f) {
        return DS18B20_ERROR;
    }
    return temp_f;
}

// --- Core 1 Entry ---
void ds18b20_core1_entry() {
    for (;;) {
        gpio_put(LED_PIN, 0); // Turn off LED
        int temp_f = ds18b20_read_temp();
        if (temp_f != DS18B20_ERROR && temp_f >= 0 && temp_f <= 255) {
            memcpy(packet_buffer, UUID_PATTERN, 16);
            packet_buffer[TEMPERATURE_BYTE_INDEX] = (uint8_t)temp_f;
            multicore_fifo_push_blocking(1);
        } else {
            printf("CRITICAL: Failed to get valid temp.\n");
        }
        gpio_put(LED_PIN, 1); // Turn on LED
        sleep_ms(10000);
    }
}

void initialize_ds18b20(void)
{
    gpio_init(ONE_WIRE_PIN);
    if (!one_wire_reset()) {
        printf("CRITICAL: No DS18B20 device found. Exiting.\n");
        while(1) {
            gpio_put(LED_PIN, 1);
            sleep_ms(500);
            gpio_put(LED_PIN, 0);
            sleep_ms(500);
        }
    }
}

