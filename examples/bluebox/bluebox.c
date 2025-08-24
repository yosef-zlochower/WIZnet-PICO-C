/*
 * UDP Sender example for W5500 and RP2040.
 *
 * This program reads a DS18B20 temperature sensor on GPIO6 using the one-wire
 * protocol on core 1. It then sends the temperature data via a UDP packet
 * on core 0 to a specified host every 10 seconds. An LED on GPIO29 blinks
 * to indicate program activity.
 *
 * This version is configured for a custom PCB with the following pinout:
 * - W5500 SPI: SPIO_SCK (GPIO2), SPIO_TX (GPIO3), SPIO_RX (GPIO4), CSn (GPIO1), RSTn (GPIO0)
 * - 1-Wire: GPIO6
 * - LED: GPIO29
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include <pico/multicore.h>
#include <pico/stdio_usb.h>

// The ioLibrary_Driver from the WIZnet-PICO-C repository
#include "wizchip_conf.h"
#include "socket.h"
#include "wizchip_spi.h"

// --- Hardware Pin Configurations ---
// These are the specific pin assignments for your custom PCB.
#define WIZ_SPI_PORT spi0
#define WIZ_SPI_RX_PIN 4
#define WIZ_SPI_SCK_PIN 2
#define WIZ_SPI_TX_PIN 3
#define WIZ_CS_PIN 1
#define WIZ_RST_PIN 0

// Temperature sensor and LED
#define ONE_WIRE_PIN 6   // GPIO6 for the DS18B20 data line
#define LED_PIN 29       // GPIO29 for an external LED

// --- Network and UDP Configuration ---
#define UDP_PORT 16216
#define SOCKET_NUM 0
#define PACKET_SIZE 17
#define TEMPERATURE_BYTE_INDEX 16

// --- One-Wire and DS18B20 Protocol ---
#define DS18B20_CONVERT_T 0x44
#define DS18B20_READ_SCRATCHPAD 0xbe
#define DS18B20_ERROR -128

static const uint8_t UUID_PATTERN[16] = {
    0xd4, 0x3f, 0x9b, 0x60, 0xec, 0x14, 0x54, 0xd6,
    0x95, 0xc9, 0x3f, 0x3a, 0x4b, 0x81, 0x6e, 0xaa
};
uint8_t packet_buffer[PACKET_SIZE];


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

    // Start temperature conversion
    if (!one_wire_reset()) return DS18B20_ERROR;
    one_wire_write_byte(0xcc); // Skip ROM
    one_wire_write_byte(DS18B20_CONVERT_T);

    // Wait for conversion to complete (up to 750ms)
    sleep_ms(750);

    // Read scratchpad
    if (!one_wire_reset()) return DS18B20_ERROR;
    one_wire_write_byte(0xcc); // Skip ROM
    one_wire_write_byte(DS18B20_READ_SCRATCHPAD);

    for (int i = 0; i < 9; i++) {
        scratchpad[i] = one_wire_read_byte();
    }

    int16_t raw_temp = (scratchpad[1] << 8) | scratchpad[0];
    float temp_c = (float)raw_temp / 16.0f;
    int temp_f = (int)(temp_c * 1.8f + 32.0f + 0.5f);

    if (temp_c < -55.0f || temp_c > 125.0f) {
        return DS18B20_ERROR; // Indicate a reading error
    }

    return temp_f;
}

// --- WIZnet SPI Callback Functions ---
static void wizchip_cs_select(void) {
    gpio_put(WIZ_CS_PIN, 0);
}

static void wizchip_cs_deselect(void) {
    gpio_put(WIZ_CS_PIN, 1);
}

static void wizchip_spi_write_byte(uint8_t data) {
    spi_write_blocking(WIZ_SPI_PORT, &data, 1);
}

static uint8_t wizchip_spi_read_byte(void) {
    uint8_t data;
    spi_read_blocking(WIZ_SPI_PORT, 0, &data, 1);
    return data;
}

static void wizchip_reset_pin_low(void) {
    gpio_put(WIZ_RST_PIN, 0);
}

static void wizchip_reset_pin_high(void) {
    gpio_put(WIZ_RST_PIN, 1);
}

// Helper function to print the current network information
// This function is defined in wizchip_spi.h and linked from the library
// void print_network_information(wiz_NetInfo net_info);

// --- Core 1 Entry ---
static void core1_entry() {
    for (;;) {
        gpio_put(LED_PIN, 0); // Turn off LED

        int temp_f = ds18b20_read_temp();

        if (temp_f != DS18B20_ERROR && temp_f >= 0 && temp_f <= 255) {
            memcpy(packet_buffer, UUID_PATTERN, 16);
            packet_buffer[TEMPERATURE_BYTE_INDEX] = (uint8_t)temp_f;
            multicore_fifo_push_blocking(1); // Signal core 0
        } else {
            printf("CRITICAL: Failed to get valid temp.\n");
        }

        gpio_put(LED_PIN, 1); // Turn on LED

        sleep_ms(10000); // Wait 10 seconds
    }
}


// Network configuration
static wiz_NetInfo g_net_info =
    {
        .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
        .ip = {192, 168, 2, 162},                    // IP address
        .sn = {255, 255, 255, 0},                    // Subnet Mask
        .gw = {192, 168, 2, 1},                      // Gateway
#if _WIZCHIP_ > W5500
        // No IPv6 settings needed for this W5500 example
#else
        .dhcp = NETINFO_STATIC
#endif
    };

// Destination IP address and port for the UDP packet
uint8_t dest_ip[4] = {192, 168, 2, 10};
uint16_t dest_port = UDP_PORT;


int main() {
    stdio_init_all();
    sleep_ms(3000); // Wait for the terminal to connect

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1); // Turn on LED to indicate activity

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

    printf("Pico temperature sender initialized. Sending data over serial...\n");

    // Explicitly initialize SPI hardware
    spi_init(WIZ_SPI_PORT, 8000 * 1000); // 8MHz SPI clock
    gpio_set_function(WIZ_SPI_RX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(WIZ_SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(WIZ_SPI_TX_PIN, GPIO_FUNC_SPI);

    // Initialize WIZnet control pins
    gpio_init(WIZ_CS_PIN);
    gpio_set_dir(WIZ_CS_PIN, GPIO_OUT);
    gpio_put(WIZ_CS_PIN, 1);
    
    gpio_init(WIZ_RST_PIN);
    gpio_set_dir(WIZ_RST_PIN, GPIO_OUT);
    gpio_put(WIZ_RST_PIN, 1);
    
    // Perform hardware reset
    wizchip_reset_pin_low();
    sleep_ms(100);
    wizchip_reset_pin_high();
    sleep_ms(100);

    // Register callback functions for the WIZnet library
    reg_wizchip_cs_cbfunc(wizchip_cs_select, wizchip_cs_deselect);
    reg_wizchip_spi_cbfunc(wizchip_spi_read_byte, wizchip_spi_write_byte);
    
    // Initialize the WIZnet chip using the configured callbacks
    wizchip_initialize();
    
    // Set network information
    network_initialize(g_net_info);
    
    // Print out the assigned network info for verification
    print_network_information(g_net_info);

    // Open a UDP socket
    if (socket(SOCKET_NUM, Sn_MR_UDP, UDP_PORT, 0) == 0) {
        printf("UDP socket opened on port %d.\n", UDP_PORT);
    } else {
        printf("Failed to open UDP socket.\n");
        return 1; // Exit with an error code
    }

    // Launch core 1 to handle temperature sensing
    multicore_launch_core1(core1_entry);

    for (;;) {
        // Wait for core 1 to push a new temperature packet
        uint32_t packet_ready = multicore_fifo_pop_blocking();
        
        if (packet_ready == 1) {
            // Print the temperature before sending
            printf("The temperature is %u F\n", packet_buffer[PACKET_SIZE-1]);
            
            // Send the UDP packet
            int32_t len = sendto(SOCKET_NUM, packet_buffer, PACKET_SIZE, dest_ip, dest_port);

            if (len > 0) {
                printf("Sent UDP packet to %d.%d.%d.%d:%d\n",
                       dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3], dest_port);
            } else {
                printf("Failed to send UDP packet. Error: %ld\n", len);
            }
        }
    }
    
    // Clean up
    close(SOCKET_NUM);
    return 0;
}

