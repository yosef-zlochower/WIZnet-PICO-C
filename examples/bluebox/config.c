/*
 * config.c
 * This file contains functions for configuring the network settings
 * via the serial console and storing them in flash memory.
 */
#include <ctype.h>
#include <pico/multicore.h>
#include <pico/stdio_usb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "pico/unique_id.h"

#include "config.h"
#include "hardware.h"
#include "led_state.h"

// Flash memory configuration for storing network settings
#define FLASH_TARGET_OFFSET (1024 * 1024)
const uint8_t *flash_target_contents =
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

// --- Internal Helper Functions ---

// Custom function to read a single hexadecimal byte with validation and echo
static int read_hex_byte()
{
    int i = 0;
    char c;
    int tval = 0;
    while (i < 2)
    {
        c = getchar();
        if (c == '\r' || c == '\n' || c == ':' || c == EOF)
        {
            if (i > 0)
                break;
            else
                return -2; // Indicates empty input
        }
        if (c == '\b' || c == 0x7F)
        { // Handle backspace
            if (i > 0)
            {
                i--;
                tval /= 16;
                printf("\b \b");
            }
            continue;
        }
        c = toupper(c);
        if (c >= '0' && c <= '9')
        {
            i++;
            tval = 16 * tval + c - '0';
            printf("%c", c);
        }
        else if (c >= 'A' && c <= 'F')
        {
            i++;
            tval = 16 * tval + 10 + c - 'A';
            printf("%c", c);
        }
    }
    return tval;
}

// Custom function to read a single decimal byte with validation and echo
static int read_decimal_byte()
{
    int i = 0;
    char c;
    int tval = 0;
    while (i < 3)
    {
        c = getchar();
        if (c == '\r' || c == '\n' || c == '.' || c == EOF)
        {
            if (i > 0)
                break;
            else
                return -2; // Indicates empty input
        }
        if (c == '\b' || c == 0x7F)
        { // Handle backspace
            if (i > 0)
            {
                i--;
                tval /= 10;
                printf("\b \b");
            }
            continue;
        }
        if (c >= '0' && c <= '9')
        {
            int nval = 10 * tval + c - '0';
            if (nval > 255)
            {
                continue;
            }
            i++;
            tval = nval;
            printf("%c", c);
        }
    }
    return tval;
}

// Custom function to read a 16-bit decimal word with validation and echo
static int read_decimal_word()
{
    int i = 0;
    char c;
    int tval = 0;
    while (i < 5)
    {
        c = getchar();
        if (c == '\r' || c == '\n' || c == EOF)
        {
            if (i > 0)
                break;
            else
                return -2; // Indicates empty input
        }
        if (c == '\b' || c == 0x7F)
        { // Handle backspace
            if (i > 0)
            {
                i--;
                tval /= 10;
                printf("\b \b");
            }
            continue;
        }
        if (c >= '0' && c <= '9')
        {
            int nval = 10 * tval + c - '0';
            if (nval > 65535)
            {
                continue;
            }
            i++;
            tval = nval;
            printf("%c", c);
        }
    }
    return tval;
}

// Helper function to get a single character with echo
static char get_confirmation()
{
    char c = getchar();
    putchar(c);
    printf("\n");
    return c;
}

// Generate a unique default MAC from the board's flash ID
void generate_default_mac(uint8_t *mac_out)
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    mac_out[0] = board_id.id[2];
    mac_out[1] = board_id.id[3];
    mac_out[2] = board_id.id[4];
    mac_out[3] = board_id.id[5];
    mac_out[4] = board_id.id[6];
    mac_out[5] = board_id.id[7];

    // Set locally-administered bit, clear multicast bit
    mac_out[0] = (mac_out[0] & 0xFC) | 0x02;
}

// --- Public API Functions ---

// Calculate a simple checksum
uint32_t calculate_checksum(const network_config_t *config)
{
    uint32_t sum = 0;
    for (int i = 0; i < sizeof(config->mac); i++)
        sum += config->mac[i];
    for (int i = 0; i < sizeof(config->ip); i++)
        sum += config->ip[i];
    for (int i = 0; i < sizeof(config->sn); i++)
        sum += config->sn[i];
    for (int i = 0; i < sizeof(config->gw); i++)
        sum += config->gw[i];
    for (int i = 0; i < sizeof(config->dest_ip); i++)
        sum += config->dest_ip[i];
    sum += config->dest_port;
    sum += config->time_delay;
    sum += config->packet_style;
    sum += config->use_dhcp;
    sum += config->route_via_gateway;
    return sum;
}

// Function to read the configuration from flash
bool read_config_from_flash(network_config_t *config)
{
    memcpy(config, flash_target_contents, sizeof(network_config_t));
    if (config->magic_number == CONFIG_MAGIC_NUMBER &&
        config->checksum == calculate_checksum(config))
    {
        printf("Successfully loaded network configuration from flash.\n");
        return true;
    }
    printf("No valid network configuration found in flash. Using defaults.\n");
    return false;
}

// Function to write the configuration to flash
void write_config_to_flash(const network_config_t *config)
{
    uint8_t buffer[FLASH_SECTOR_SIZE];
    memcpy(buffer, config, sizeof(network_config_t));

    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_SECTOR_SIZE);
    restore_interrupts(interrupts);

    printf("Network configuration saved to flash.\n");
}

// Function to set up network config via console with validation
void setup_network_via_console(network_config_t *net_config)
{
    uint8_t temp_mac[6];
    uint8_t temp_ip[4];
    uint8_t temp_sn[4];
    uint8_t temp_gw[4];
    uint8_t temp_dest_ip[4];
    int val;
    char confirm;


    enter_config_state();

    do
    {
        printf("\n Press Enter to start configuration\n");
        char c = getchar_timeout_us(5000000);
        if (c == '\n' || c == '\r')
        {
            break;
        }
    } while (1);

    printf("\n--- Entering Network Configuration Mode ---\n");

    {
        uint8_t default_mac[6];
        generate_default_mac(default_mac);
        printf(
            "MAC address (current: %02X:%02X:%02X:%02X:%02X:%02X)\n",
            net_config->mac[0], net_config->mac[1], net_config->mac[2],
            net_config->mac[3], net_config->mac[4], net_config->mac[5]);
        printf("  board unique default: %02X:%02X:%02X:%02X:%02X:%02X\n",
            default_mac[0], default_mac[1], default_mac[2],
            default_mac[3], default_mac[4], default_mac[5]);

        do
        {
            printf("Use (c)urrent, (u)nique default, or (m)anual entry? ");
            char choice = getchar();
            putchar(choice);
            printf("\n");
            if (choice == 'c' || choice == 'C')
            {
                break;
            }
            else if (choice == 'u' || choice == 'U')
            {
                memcpy(net_config->mac, default_mac, 6);
                printf("MAC set to board default: "
                       "%02X:%02X:%02X:%02X:%02X:%02X\n",
                       net_config->mac[0], net_config->mac[1],
                       net_config->mac[2], net_config->mac[3],
                       net_config->mac[4], net_config->mac[5]);
                break;
            }
            else if (choice == 'm' || choice == 'M')
            {
                do
                {
                    printf("Enter MAC (XX:XX:XX:XX:XX:XX): ");
                    temp_mac[0] = read_hex_byte();
                    if (temp_mac[0] == -2)
                        continue;
                    printf(":");
                    temp_mac[1] = read_hex_byte();
                    printf(":");
                    temp_mac[2] = read_hex_byte();
                    printf(":");
                    temp_mac[3] = read_hex_byte();
                    printf(":");
                    temp_mac[4] = read_hex_byte();
                    printf(":");
                    temp_mac[5] = read_hex_byte();

                    if (temp_mac[1] != -2 && temp_mac[2] != -2 &&
                        temp_mac[3] != -2 && temp_mac[4] != -2 &&
                        temp_mac[5] != -2)
                    {
                        printf("\nNew MAC: %02X:%02X:%02X:%02X:%02X:%02X. "
                               "Is this correct? (y/n): ",
                               temp_mac[0], temp_mac[1], temp_mac[2],
                               temp_mac[3], temp_mac[4], temp_mac[5]);
                        confirm = get_confirmation();
                        if (confirm == 'y' || confirm == 'Y')
                        {
                            memcpy(net_config->mac, temp_mac, 6);
                            break;
                        }
                    }
                    else
                    {
                        printf("\nInvalid MAC address. Please use the format "
                               "XX:XX:XX:XX:XX:XX.\n");
                    }
                } while (1);
                break;
            }
            else
            {
                printf("Invalid choice. Please enter 'c', 'u', or 'm'.\n");
            }
        } while (1);
    }

    do
    {
        printf("\nUse DHCP for IP/subnet/gateway? (current: %s)\n",
               net_config->use_dhcp ? "yes" : "no");
        printf("Enter y or n, or press return to accept current: ");
        char dhcp_choice = getchar();
        if (dhcp_choice == '\r' || dhcp_choice == '\n')
        {
            break;
        }
        putchar(dhcp_choice);
        // consume trailing newline
        char trail = getchar();
        (void)trail;
        if (dhcp_choice == 'y' || dhcp_choice == 'Y')
        {
            printf("\nDHCP enabled.\n");
            net_config->use_dhcp = 1;
            break;
        }
        else if (dhcp_choice == 'n' || dhcp_choice == 'N')
        {
            printf("\nDHCP disabled. Using static IP configuration.\n");
            net_config->use_dhcp = 0;
            break;
        }
        else
        {
            printf("\nInvalid input. Please enter 'y' or 'n'.\n");
        }
    } while (1);

    if (!net_config->use_dhcp)
    {
    do
    {
        printf("\nEnter new IP address (current: %d.%d.%d.%d)\n",
               net_config->ip[0], net_config->ip[1], net_config->ip[2],
               net_config->ip[3]);
        printf("or press return to accept current: ");
        val = read_decimal_byte();
        if (val == -2)
            break;
        temp_ip[0] = val;
        printf(".");
        temp_ip[1] = read_decimal_byte();
        printf(".");
        temp_ip[2] = read_decimal_byte();
        printf(".");
        temp_ip[3] = read_decimal_byte();
        if (temp_ip[0] != -2 && temp_ip[1] != -2 && temp_ip[2] != -2 &&
            temp_ip[3] != -2)
        {
            printf("\nNew IP: %d.%d.%d.%d. Is this correct? (y/n): ",
                   temp_ip[0], temp_ip[1], temp_ip[2], temp_ip[3]);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                memcpy(net_config->ip, temp_ip, 4);
                break;
            }
        }
        else
        {
            printf("\nInvalid IP address. Please use the format X.X.X.X.\n");
        }
    } while (1);

    do
    {
        printf("\nEnter new Subnet Mask (current: %d.%d.%d.%d)\n",
               net_config->sn[0], net_config->sn[1], net_config->sn[2],
               net_config->sn[3]);
        printf("or press return to accept current: ");
        val = read_decimal_byte();
        if (val == -2)
            break;
        temp_sn[0] = val;
        printf(".");
        temp_sn[1] = read_decimal_byte();
        printf(".");
        temp_sn[2] = read_decimal_byte();
        printf(".");
        temp_sn[3] = read_decimal_byte();
        if (temp_sn[0] != -2 && temp_sn[1] != -2 && temp_sn[2] != -2 &&
            temp_sn[3] != -2)
        {
            printf("\nNew Subnet: %d.%d.%d.%d. Is this correct? (y/n): ",
                   temp_sn[0], temp_sn[1], temp_sn[2], temp_sn[3]);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                memcpy(net_config->sn, temp_sn, 4);
                break;
            }
        }
        else
        {
            printf("\nInvalid Subnet Mask. Please use the format X.X.X.X.\n");
        }
    } while (1);

    do
    {
        printf("\nEnter new Gateway (current: %d.%d.%d.%d)\n",
               net_config->gw[0], net_config->gw[1], net_config->gw[2],
               net_config->gw[3]);
        printf("or press return to accept current: ");
        val = read_decimal_byte();
        if (val == -2)
            break;
        temp_gw[0] = val;
        printf(".");
        temp_gw[1] = read_decimal_byte();
        printf(".");
        temp_gw[2] = read_decimal_byte();
        printf(".");
        temp_gw[3] = read_decimal_byte();
        if (temp_gw[0] != -2 && temp_gw[1] != -2 && temp_gw[2] != -2 &&
            temp_gw[3] != -2)
        {
            printf("\nNew Gateway: %d.%d.%d.%d. Is this correct? (y/n): ",
                   temp_gw[0], temp_gw[1], temp_gw[2], temp_gw[3]);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                memcpy(net_config->gw, temp_gw, 4);
                break;
            }
        }
        else
        {
            printf("\nInvalid Gateway. Please use the format X.X.X.X.\n");
        }
    } while (1);
    } // end if (!net_config->use_dhcp)

    do
    {
        printf("\nRoute all traffic via the gateway? (current: %s)\n",
               net_config->route_via_gateway ? "yes" : "no");
        printf("  (Use when the network blocks direct host-to-host traffic,\n"
               "   e.g. switch port/client isolation. Forces a /32 mask so\n"
               "   the chip ARPs only the gateway and the router relays.)\n");
        printf("Enter y or n, or press return to accept current: ");
        char gw_choice = getchar();
        if (gw_choice == '\r' || gw_choice == '\n')
        {
            break;
        }
        putchar(gw_choice);
        // consume trailing newline
        char trail = getchar();
        (void)trail;
        if (gw_choice == 'y' || gw_choice == 'Y')
        {
            printf("\nGateway routing enabled.\n");
            net_config->route_via_gateway = 1;
            break;
        }
        else if (gw_choice == 'n' || gw_choice == 'N')
        {
            printf("\nGateway routing disabled.\n");
            net_config->route_via_gateway = 0;
            break;
        }
        else
        {
            printf("\nInvalid input. Please enter 'y' or 'n'.\n");
        }
    } while (1);

    do
    {
        printf("\nEnter new Destination IP (current: %d.%d.%d.%d)\n",
               net_config->dest_ip[0], net_config->dest_ip[1],
               net_config->dest_ip[2], net_config->dest_ip[3]);
        printf("or press return to accept current: ");
        val = read_decimal_byte();
        if (val == -2)
            break;
        temp_dest_ip[0] = val;
        printf(".");
        temp_dest_ip[1] = read_decimal_byte();
        printf(".");
        temp_dest_ip[2] = read_decimal_byte();
        printf(".");
        temp_dest_ip[3] = read_decimal_byte();
        if (temp_dest_ip[0] != -2 && temp_dest_ip[1] != -2 &&
            temp_dest_ip[2] != -2 && temp_dest_ip[3] != -2)
        {
            printf(
                "\nNew Destination IP: %d.%d.%d.%d. Is this correct? (y/n): ",
                temp_dest_ip[0], temp_dest_ip[1], temp_dest_ip[2],
                temp_dest_ip[3]);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                memcpy(net_config->dest_ip, temp_dest_ip, 4);
                break;
            }
        }
        else
        {
            printf(
                "\nInvalid Destination IP. Please use the format X.X.X.X.\n");
        }
    } while (1);

    do
    {
        printf("\nEnter new Destination Port (current: %u)\n",
               net_config->dest_port);
        printf("or press return to accept current: ");
        val = read_decimal_word();
        if (val == -2)
            break;
        if (val != -1)
        {
            printf("\nNew Destination Port: %u. Is this correct? (y/n): ", val);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                net_config->dest_port = val;
                break;
            }
        }
        else
        {
            printf("\nInvalid Destination Port. Please enter a valid port "
                   "number.\n");
        }
    } while (1);

    do
    {
        printf("\nEnter time delay between between temperature readings "
               "(current: %u)\n",
               net_config->time_delay);
        printf("or press return to accept current: ");
        val = read_decimal_word();
        if (val == -2)
            break;
        if (val != -1)
        {
            if (val < 1)
            {
                val = 1;
            }
            printf("\nTime delay: %u (s). Is this correct? (y/n): ", val);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                net_config->time_delay = val;
                break;
            }
        }
        else
        {
            printf("\nInvalid time delay. Please enter a valid time "
                   "delay.\n");
        }
    } while (1);

    do
    {
        printf("\nEnter  packet style (0=new style, 1=old trap style)"
               "(current: %u)\n",
               net_config->packet_style);
        printf("or press return to accept current: ");
        val = read_decimal_byte();
        if (val == -2)
            break;
        if (val == 0 || val == 1)
        {
            printf("\nPacket type =  %u (s). Is this correct? (y/n): ", val);
            confirm = get_confirmation();
            if (confirm == 'y' || confirm == 'Y')
            {
                net_config->packet_style = val;
                break;
            }
        }
        else
        {
            printf("\nInvalid packet_style. Please enter a valid type (0 or 1)\n");
        }
    } while (1);


    leave_config_state();

    // Set magic number and checksum before writing
    net_config->magic_number = CONFIG_MAGIC_NUMBER;
    net_config->checksum = calculate_checksum(net_config);

    // Write to flash
    write_config_to_flash(net_config);
    gpio_put(LED_PIN, 1);
}
