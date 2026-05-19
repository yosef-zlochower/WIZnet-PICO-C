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

#include "dhcp.h"
#include "timer.h"

#include "config.h"
#include "hardware.h"
#include "network.h"
#include "led_state.h"

// --- DHCP State ---
#define ETHERNET_BUF_MAX_SIZE (1024 * 2)
#define DHCP_RETRY_COUNT 5

static uint8_t g_dhcp_buf[ETHERNET_BUF_MAX_SIZE];
static volatile uint16_t g_msec_cnt = 0;
static bool g_dhcp_active = false;
static bool g_dhcp_got_ip = false;

static void dhcp_timer_callback(void)
{
    g_msec_cnt++;
    if (g_msec_cnt >= 1000)
    {
        g_msec_cnt = 0;
        DHCP_time_handler();
    }
}

static void dhcp_assign_cb(void)
{
    wiz_NetInfo net_info;
    getIPfromDHCP(net_info.ip);
    getGWfromDHCP(net_info.gw);
    getSNfromDHCP(net_info.sn);
    getDNSfromDHCP(net_info.dns);
    // ctlnetwork(CN_SET_NETINFO) also writes the chip's MAC (SHAR) from
    // net_info.mac. DHCP doesn't supply a MAC, so read back the one already
    // programmed by network_setup() instead of leaving it as stack garbage.
    getSHAR(net_info.mac);

    ctlnetwork(CN_SET_NETINFO, &net_info);

    printf("\n--- DHCP Configuration ---\n");
    printf("  IP:         %d.%d.%d.%d\n",
           net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    printf("  Subnet:     %d.%d.%d.%d\n",
           net_info.sn[0], net_info.sn[1], net_info.sn[2], net_info.sn[3]);
    printf("  Gateway:    %d.%d.%d.%d\n",
           net_info.gw[0], net_info.gw[1], net_info.gw[2], net_info.gw[3]);
    printf("  DNS:        %d.%d.%d.%d\n",
           net_info.dns[0], net_info.dns[1], net_info.dns[2], net_info.dns[3]);
    printf("  Lease time: %lu seconds\n", getDHCPLeasetime());
    printf("--------------------------\n");
}

static void dhcp_conflict_cb(void)
{
    printf("DHCP IP conflict detected!\n");
}

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

#if (DEVICE_BOARD_NAME == BLUEBOX_W5500)
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
#elif (DEVICE_BOARD_NAME == BLUEBOX_W55RP20)
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
#else
    #error Unknown device
#endif

    // Initialize the WIZnet chip using the configured callbacks
    wizchip_initialize();

    if (config.use_dhcp)
    {
        // For DHCP: set MAC address on the chip, then start DHCP client
        wiz_NetInfo net_info = {.dns = {8, 8, 8, 8}};
        memcpy(net_info.mac, config.mac, 6);
        memcpy(net_info.ip, (uint8_t[]){0, 0, 0, 0}, 4);
        memcpy(net_info.sn, (uint8_t[]){0, 0, 0, 0}, 4);
        memcpy(net_info.gw, (uint8_t[]){0, 0, 0, 0}, 4);
#if _WIZCHIP_ > W5500
#else
        net_info.dhcp = NETINFO_DHCP;
#endif
        network_initialize(net_info);

        wizchip_1ms_timer_initialize(dhcp_timer_callback);
        DHCP_init(SOCKET_DHCP, g_dhcp_buf);
        reg_dhcp_cbfunc(dhcp_assign_cb, dhcp_assign_cb, dhcp_conflict_cb);
        g_dhcp_active = true;

        printf("DHCP client initialized.\n");
    }
    else
    {
        // Static IP configuration
        wiz_NetInfo net_info = {.dns = {8, 8, 8, 8}};
        memcpy(net_info.mac, config.mac, 6);
        memcpy(net_info.ip, config.ip, 4);
        memcpy(net_info.sn, config.sn, 4);
        memcpy(net_info.gw, config.gw, 4);
#if _WIZCHIP_ > W5500
#else
        net_info.dhcp = NETINFO_STATIC;
#endif
        network_initialize(net_info);
        print_network_information(net_info);
    }

    leave_error_state();
}

// Run DHCP until IP is obtained or max retries exceeded
bool network_dhcp_run(void)
{
    if (!g_dhcp_active)
        return true; // static IP, nothing to do

    uint8_t dhcp_retry = 0;
    g_dhcp_got_ip = false;

    printf("Waiting for DHCP...\n");

    while (1)
    {
        uint8_t ret = DHCP_run();

        if (ret == DHCP_IP_LEASED)
        {
            if (!g_dhcp_got_ip)
            {
                printf("DHCP success.\n");
                g_dhcp_got_ip = true;
            }
            return true;
        }
        else if (ret == DHCP_IP_ASSIGN || ret == DHCP_IP_CHANGED)
        {
            g_dhcp_got_ip = true;
            return true;
        }
        else if (ret == DHCP_FAILED)
        {
            dhcp_retry++;
            printf("DHCP timeout, retry %d/%d\n", dhcp_retry, DHCP_RETRY_COUNT);

            if (dhcp_retry > DHCP_RETRY_COUNT)
            {
                printf("DHCP failed after %d retries.\n", DHCP_RETRY_COUNT);
                DHCP_stop();
                g_dhcp_active = false;
                return false;
            }
        }

        sleep_ms(100);
    }
}

// Call periodically for DHCP lease renewal
void network_dhcp_maintain(void)
{
    if (!g_dhcp_active)
        return;

    uint8_t ret = DHCP_run();
    if (ret == DHCP_FAILED)
    {
        printf("DHCP lease renewal failed. Attempting re-init.\n");
        DHCP_init(SOCKET_DHCP, g_dhcp_buf);
        reg_dhcp_cbfunc(dhcp_assign_cb, dhcp_assign_cb, dhcp_conflict_cb);
    }
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
