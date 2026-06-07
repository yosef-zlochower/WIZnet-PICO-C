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
#include "mac.h"
#include "network.h"
#include "led_state.h"

// --- Packet Buffer and Network Configuration ---
uint8_t packet_pattern[PACKET_SIZE_MAX];
uint16_t packet_size;
uint16_t temperature_byte_index;
uint8_t packet_buffer[PACKET_SIZE_MAX];
uint8_t dest_ip_global[4];
uint16_t dest_port_global;
uint16_t time_delay_global;
uint8_t packet_style = 0;

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

    // Probe the optional MAC EEPROM (24AA02E48) once, before any caller asks.
    mac_init();

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
            if (!mac_eeprom_get(net_config.mac))
                mac_random_generate(net_config.mac);
            memcpy(net_config.ip, (uint8_t[]){192, 168, 2, 162}, 4);
            memcpy(net_config.sn, (uint8_t[]){255, 255, 255, 0}, 4);
            memcpy(net_config.gw, (uint8_t[]){192, 168, 2, 1}, 4);
            memcpy(net_config.dest_ip, (uint8_t[]){192, 168, 2, 10}, 4);
            net_config.dest_port = 16216;
            net_config.time_delay = 10;
            net_config.packet_style = 0;
            net_config.use_dhcp = 0;
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
        packet_style = net_config.packet_style;
    }
    else
    {
        // Use default config
        printf("Using default configuration.\n");
        network_config_t default_config = {
            .ip = {192, 168, 2, 162},
            .sn = {255, 255, 255, 0},
            .gw = {192, 168, 2, 1},
            .dest_ip = {192, 168, 2, 10},
            .dest_port = 16216,
            .time_delay = 10,
            .packet_style = 0,
            .use_dhcp = 0,
        };
        if (!mac_eeprom_get(default_config.mac))
        {
            // No EEPROM: freshly randomize, then persist so the MAC is stable
            // across reboots. With an EEPROM present we skip this — the chip
            // itself is the stable source of truth.
            mac_random_generate(default_config.mac);
            printf("No MAC EEPROM; persisting generated random MAC to flash.\n");
            default_config.magic_number = CONFIG_MAGIC_NUMBER;
            default_config.checksum = calculate_checksum(&default_config);
            write_config_to_flash(&default_config);
        }
        network_setup(default_config);
        memcpy(dest_ip_global, default_config.dest_ip, 4);
        dest_port_global = default_config.dest_port;
        time_delay_global = default_config.time_delay;
        packet_style = default_config.packet_style;
    }

    // If DHCP is enabled, obtain IP before proceeding
    if (!network_dhcp_run())
    {
        printf("ERROR: Failed to obtain IP via DHCP\n");
        enter_error_state();
        do {} while(1);
    }

    if (packet_style == 0)
    {
        packet_size = PACKET_SIZE_NEW;
        uint8_t pattern [] = UDP_PACKET_PATTERN_NEW;
        memcpy(packet_pattern, pattern, PACKET_SIZE_NEW);
        temperature_byte_index = TEMPERATURE_BYTE_INDEX_NEW;
    }
    else if (packet_style == 1)
    {
        packet_size = PACKET_SIZE_OLD;
        uint8_t pattern [] = UDP_PACKET_PATTERN_OLD;
        memcpy(packet_pattern, pattern, PACKET_SIZE_OLD);
        temperature_byte_index = TEMPERATURE_BYTE_INDEX_OLD;
    }
    else
    {
        printf("ERROR: INVALID PACKET_STYLE\n");
        enter_error_state();
        do {} while(1);
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
            // Maintain DHCP lease (no-op if static IP)
            network_dhcp_maintain();

            printf("The temperature is %u F\n",
                   packet_buffer[temperature_byte_index]);

            // Send the UDP packet
            int32_t len = network_send_packet(packet_buffer, packet_size,
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
