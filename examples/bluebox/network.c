/*
 * network.c
 * This file contains the WIZnet-specific network functions, including
 * SPI callbacks and chip initialization routines.
 */
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#include "socket.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"

#include "config.h"
#include "hardware.h"
#include "network.h"
#include "led_state.h"

// --- WIZnet SPI Callback Functions ---
static void wizchip_cs_select(void) { gpio_put(WIZ_CS_PIN, 0); }

static void wizchip_cs_deselect(void) { gpio_put(WIZ_CS_PIN, 1); }

static void wizchip_spi_write_byte(uint8_t data)
{
    spi_write_blocking(WIZ_SPI_PORT, &data, 1);
}

static uint8_t wizchip_spi_read_byte(void)
{
    uint8_t data;
    spi_read_blocking(WIZ_SPI_PORT, 0, &data, 1);
    return data;
}

static void wizchip_reset_pin_low(void) { gpio_put(WIZ_RST_PIN, 0); }

static void wizchip_reset_pin_high(void) { gpio_put(WIZ_RST_PIN, 1); }

// --- Public API Functions ---

// Initialize network hardware and software
void network_setup(network_config_t config)
{
    enter_error_state();
    // Explicitly initialize SPI hardware
    spi_init(WIZ_SPI_PORT, 8000 * 1000);
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

    // Set network information from the provided config
    wiz_NetInfo net_info;
    memcpy(net_info.mac, config.mac, 6);
    memcpy(net_info.ip, config.ip, 4);
    memcpy(net_info.sn, config.sn, 4);
    memcpy(net_info.gw, config.gw, 4);
#if _WIZCHIP_ > W5500
#else
    net_info.dhcp = NETINFO_STATIC;
#endif

    network_initialize(net_info);

    // Print out the assigned network info for verification
    print_network_information(net_info);
    leave_error_state();

}

// Open a UDP socket
int network_open_socket(uint16_t local_port)
{
    int result = socket(SOCKET_NUM, Sn_MR_UDP, local_port, 0);
    if (result == 0)
    {
        printf("UDP socket opened on port %d.\n", local_port);
    }
    else
    {
        printf("Failed to open UDP socket. Error: %d\n", result);
    }
    return result;
}

void network_close_socket(void) { close(SOCKET_NUM); }

// Send a UDP packet
int32_t network_send_packet(uint8_t *packet, uint16_t size, uint8_t *dest_ip,
                            uint16_t dest_port)
{
    return sendto(SOCKET_NUM, packet, size, dest_ip, dest_port);
}
