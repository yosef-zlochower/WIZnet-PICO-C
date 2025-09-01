/*
 * main.c
 * This file contains the main program logic, including the setup
 * for the multicore architecture and the main application loop.
 */
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <pico/multicore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ds18b20.h"
#include "globals.h"
#include "hardware.h"
#include "network.h"

// --- Packet Buffer and Network Configuration ---
#define PACKET_SIZE 17
#define TEMPERATURE_BYTE_INDEX 16
uint8_t packet_buffer[PACKET_SIZE];
uint8_t dest_ip_global[4];
uint16_t dest_port_global;
uint16_t time_delay_global;

// Main application loop for core 0
int main()
{
    stdio_init_all();

    // Initialize LED and config button pins
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    gpio_init(CONFIG_BUTTON_PIN);
    gpio_set_dir(CONFIG_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(CONFIG_BUTTON_PIN);

    printf("Pico temperature sender initialized.\n");

    // Handle network configuration
    network_config_t net_config;
    bool config_loaded = false;

    if (gpio_get(CONFIG_BUTTON_PIN) == 0)
    {
        printf("Config button pressed. Entering configuration mode.\n");
        bool valid = read_config_from_flash(&net_config);
        if (!valid)
        {
            // Initialize with default configuration to display in the prompt
            memcpy(net_config.mac,
                   (uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02}, 6);
            memcpy(net_config.ip, (uint8_t[]){192, 168, 2, 162}, 4);
            memcpy(net_config.sn, (uint8_t[]){255, 255, 255, 0}, 4);
            memcpy(net_config.gw, (uint8_t[]){192, 168, 2, 1}, 4);
            memcpy(net_config.dest_ip, (uint8_t[]){192, 168, 2, 10}, 4);
            net_config.dest_port = 16216;
            net_config.time_delay = 10;
        }
        setup_network_via_console(&net_config);
        config_loaded = true;
    }
    else
    {
        config_loaded = read_config_from_flash(&net_config);
    }

    // Use either the loaded config or defaults
    if (config_loaded)
    {
        // Use loaded config
        network_setup(net_config);
        memcpy(dest_ip_global, net_config.dest_ip, 4);
        dest_port_global = net_config.dest_port;
        time_delay_global = net_config.time_delay;
    }
    else
    {
        // Use default config
        printf("Using default configuration.\n");
        network_config_t default_config = {
            .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
            .ip = {192, 168, 2, 162},
            .sn = {255, 255, 255, 0},
            .gw = {192, 168, 2, 1}};
        network_setup(default_config);
        memcpy(dest_ip_global, (uint8_t[]){192, 168, 2, 10}, 4);
        dest_port_global = 16216;
        time_delay_global = 10;
    }

    initialize_ds18b20();
    network_open_socket(dest_port_global);

    // Launch core 1 to handle temperature sensing
    multicore_launch_core1(ds18b20_core1_entry);

    for (;;)
    {
        // Wait for core 1 to push a new temperature packet
        uint32_t packet_ready = multicore_fifo_pop_blocking();

        if (packet_ready == 1)
        {
            printf("The temperature is %u F\n",
                   packet_buffer[TEMPERATURE_BYTE_INDEX]);

            // Send the UDP packet
            int32_t len = network_send_packet(packet_buffer, PACKET_SIZE,
                                              dest_ip_global, dest_port_global);

            if (len > 0)
            {
                printf("Sent UDP packet to %d.%d.%d.%d:%d\n", dest_ip_global[0],
                       dest_ip_global[1], dest_ip_global[2], dest_ip_global[3],
                       dest_port_global);
            }
            else
            {
                printf("Failed to send UDP packet. Error: %ld\n", len);
            }
        }
    }

    network_close_socket();
    return 0;
}
