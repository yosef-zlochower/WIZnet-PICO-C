/*
 * UDP Sender example for W55RP20-EVB-PICO.
 * This program sends a UDP packet with the string "Hello World"
 * to a specified host every 10 seconds and blinks an LED on GPIO3.
 *
 * This code is intended to be placed in a subdirectory named 'udp_sender_blink'
 * with the corresponding CMakeLists.txt file.
 *
 * It uses the libraries from the WIZnet-PICO-C repository,
 * specifically ioLibrary_Driver, and avoids lwip.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "port_common.h"



// The ioLibrary_Driver from the WIZnet-PICO-C repository
#include "wizchip_conf.h"
#include "socket.h"
#include "wizchip_spi.h"

// Macros for the LED pin and the W5500 control pins
#define LED_PIN 3
#define WIZ_RST_PIN 20
#define WIZ_CS_PIN 17

#define UDP_PORT 5000
#define SOCKET_NUM 0
#define PACKET_SIZE 12

// Network configuration
// Note: This matches the structure of the `udp_server` example
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
    // Initialize standard I/O (UART) for printing debug messages
    stdio_init_all();
    sleep_ms(3000); // Wait for the terminal to connect

    // Initialize the LED GPIO pin
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_put(LED_PIN, 1);
    printf("Starting W55RP20 UDP Sender...\n");

    // Initialize the WIZnet chip using library functions
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
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

    uint8_t msg[] = "Hello World";
    int led_state = 0;

    while (1) {
        // Toggle the LED
        led_state = !led_state;
        gpio_put(LED_PIN, led_state);

        // Send the UDP packet
        int32_t len = sendto(SOCKET_NUM, msg, PACKET_SIZE, dest_ip, dest_port);

        if (len > 0) {
            printf("Sent UDP packet: 'Hello World' to %d.%d.%d.%d:%d\n",
                   dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3], dest_port);
        } else {
            printf("Failed to send UDP packet. Error: %ld\n", len);
        }

        // Wait for 10 seconds
        sleep_ms(10000);
    }

    // Clean up (this part of the code is unreachable in the current infinite loop,
    // but is included for completeness)
    close(SOCKET_NUM);
    return 0;
}

