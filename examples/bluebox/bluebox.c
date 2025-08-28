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
 * - Config Button: GPIO28
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include <pico/multicore.h>
#include <pico/stdio_usb.h>
#include "hardware/flash.h"

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
#define CONFIG_BUTTON_PIN 28 // GPIO28 for the configuration button

// --- Network and UDP Configuration ---
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

// Flash memory configuration for storing network settings
#define FLASH_TARGET_OFFSET (1024 * 1024) // Offset for a 4KB sector at the end of flash
const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);

// Data structure to hold network configuration
typedef struct {
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t sn[4];
    uint8_t gw[4];
    uint8_t dest_ip[4];
    uint16_t dest_port;
    uint32_t checksum; // Simple checksum to check for valid data
    uint32_t magic_number; // Additional check for valid data
} network_config_t;

// A unique number to identify valid configuration data in flash
#define CONFIG_MAGIC_NUMBER 0x9E7F6B3D

// Calculate a simple checksum
uint32_t calculate_checksum(const network_config_t *config) {
    uint32_t sum = 0;
    for (int i = 0; i < sizeof(config->mac); i++) sum += config->mac[i];
    for (int i = 0; i < sizeof(config->ip); i++) sum += config->ip[i];
    for (int i = 0; i < sizeof(config->sn); i++) sum += config->sn[i];
    for (int i = 0; i < sizeof(config->gw); i++) sum += config->gw[i];
    for (int i = 0; i < sizeof(config->dest_ip); i++) sum += config->dest_ip[i];
    sum += config->dest_port;
    return sum;
}

// Function to read the configuration from flash
bool read_config_from_flash(network_config_t *config) {
    // Read the flash memory into the struct
    memcpy(config, flash_target_contents, sizeof(network_config_t));
    // Verify the checksum and magic number
    if (config->magic_number == CONFIG_MAGIC_NUMBER && config->checksum == calculate_checksum(config)) {
        printf("Successfully loaded network configuration from flash.\n");
        return true;
    }
    printf("No valid network configuration found in flash. Using defaults.\n");
    return false;
}

// Custom function to read a line from stdin with echo and backspace handling
int read_hex_byte() {
    int i = 0;
    char c;
    int tval = 0;
    while (i < 2) {
        c = getchar();
        if (c == '\r' || c == '\n' || c == ':') {
	    if ( i > 0){
                break;
	    }
        }
        if ((c == '\r' || c == '\n') && i == 0) {
	  return -1;
        }

        if (c == ':' && i == 0) {
	  continue;
        }


        if (c == '\b' || c == 0x7F) { // Handle backspace
            if (i > 0) {
                i--;
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
	else
	{
          continue;
	}
    }
    return tval;
}

int read_decimal_byte() {
    int i = 0;
    char c;
    int tval = 0;
    while (i < 3) {
        c = getchar();
        if ((c == '\r' || c == '\n' || c == '.') && i > 0) {
            break;
        }
        if ((c == '\r' || c == '\n') && i == 0) {
	  return -1;
        }
        if (c == '.' && i == 0) {
	  continue;
        }
     
        if (c == '\b' || c == 0x7F) { // Handle backspace
            if (i > 0) {
                i--;
		tval /= 10;
                printf("\b \b");
            }
            continue;
        }
	c = toupper(c);
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
	else
	{
          continue;
	}
    }
    return tval;
}

int read_decimal_word() {
    int i = 0;
    char c;
    int tval = 0;
    while (i < 5) {
        c = getchar();
        if ((c == '\r' || c == '\n') && i > 0) {
            break;
        }
        if ((c == '\r' || c == '\n') && i == 0) {
	  return -1;
        }
     
        if (c == '\b' || c == 0x7F) { // Handle backspace
            if (i > 0) {
                i--;
		tval /= 10;
                printf("\b \b");
            }
            continue;
        }
	c = toupper(c);
	if (c >= '0' && c <= '9')
        {
	  int nval = 10 * tval + c - '0';
	  if (nval > (1<<16) -1 )
          {
            continue;
          }
	  i++;
	  tval = nval;
	  printf("%c", c);
	}
	else
	{
          continue;
	}
    }
    return tval;
}



// Function to write the configuration to flash
void write_config_to_flash(const network_config_t *config) {
    // Allocate a buffer for the flash sector and copy the config
    uint8_t buffer[FLASH_SECTOR_SIZE];
    memcpy(buffer, config, sizeof(network_config_t));

    // Disable interrupts before writing to flash
    uint32_t interrupts = save_and_disable_interrupts();

    // Erase the flash sector
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);

    // Program the new data to the flash sector
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_SECTOR_SIZE);

    // Re-enable interrupts
    restore_interrupts(interrupts);

    printf("Network configuration saved to flash.\n");
}

// Custom function to read a line from stdin with echo and backspace handling
void read_line(char *buffer, int max_len) {
    int i = 0;
    char c;
    while (i < max_len - 1) {
        c = getchar();
        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        }
        if (c == '\b' || c == 0x7F) { // Handle backspace
            if (i > 0) {
                i--;
                printf("\b \b");
            }
            continue;
        }
        buffer[i++] = c;
        putchar(c);
    }
    buffer[i] = '\0';
}

// Function to set up network config via console with validation
void setup_network_via_console(network_config_t *net_config) {
    uint8_t temp_mac[6];
    uint8_t temp_ip[4];
    uint8_t temp_sn[4];
    uint8_t temp_gw[4];
    uint8_t temp_dest_ip[4];

    while (1) {
        printf("Enter new  MAC address or press return \n"
	      "to accept the current MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               net_config->mac[0], net_config->mac[1], net_config->mac[2], net_config->mac[3], net_config->mac[4], net_config->mac[5]);
	int val = read_hex_byte();
	if ( val == -1)
	{
	  break;
	}
	temp_mac[0] = val;
	printf(":");
	for (int i = 1; i < 6; i++)
	{
	  int val = read_hex_byte();
	  val = val > -1 ? val : 0;
	  temp_mac[i] = val;
	  if (i < 5)  printf(":");
	}
        memcpy(net_config->mac, temp_mac, 6);
	printf("\n");
        //printf("NEW MAC address  is %02X:%02X:%02X:%02X:%02X:%02X\n",
               //net_config->mac[0], net_config->mac[1], net_config->mac[2], net_config->mac[3], net_config->mac[4], net_config->mac[5]);

    }


    while (1) {
        printf("Enter new IP address or press return\n"
	       "to accept the current address %d.%d.%d.%d\n",
               net_config->ip[0], net_config->ip[1], net_config->ip[2], net_config->ip[3]);

	int val = read_decimal_byte();
	if ( val == -1)
	{
	  break;
	}
	temp_ip[0] = val;
	printf(".");

        for (int i = 1; i < 4; i++)
	{
	  temp_ip[i] = read_decimal_byte();
	  if (i < 4)  printf(".");
	}
        memcpy(net_config->ip, temp_ip, 4);
	printf("\n");
    }
    while (1) {
        printf("Enter new netmask  or press return\n"
	       "to accept the current netmask %d.%d.%d.%d\n",
               net_config->sn[0], net_config->sn[1], net_config->sn[2], net_config->sn[3]);

	int val = read_decimal_byte();
	if ( val == -1)
	{
	  break;
	}
	temp_sn[0] = val;
	printf(".");

        for (int i = 1; i < 4; i++)
	{
	  temp_sn[i] = read_decimal_byte();
	  if (i < 4)  printf(".");
	}
        memcpy(net_config->sn, temp_sn, 4);
	printf("\n");
    }


    while (1) {
        printf("Enter new gateway  or press return\n"
	       "to accept the current netmask %d.%d.%d.%d\n",
               net_config->gw[0], net_config->gw[1], net_config->gw[2], net_config->gw[3]);

	int val = read_decimal_byte();
	if ( val == -1)
	{
	  break;
	}
	temp_gw[0] = val;
	printf(".");

        for (int i = 1; i < 4; i++)
	{
	  temp_gw[i] = read_decimal_byte();
	  if (i < 4)  printf(".");
	}
        memcpy(net_config->gw, temp_gw, 4);
	printf("\n");
    }

    while (1) {
        printf("Enter new destination ip  or press return\n"
	       "to accept the current destination ip %d.%d.%d.%d\n",
               net_config->dest_ip[0], net_config->dest_ip[1], net_config->dest_ip[2], net_config->dest_ip[3]);

	int val = read_decimal_byte();
	if ( val == -1)
	{
	  break;
	}
	temp_dest_ip[0] = val;
	printf(".");

        for (int i = 1; i < 4; i++)
	{
	  temp_dest_ip[i] = read_decimal_byte();
	  if (i < 4)  printf(".");
	}
        memcpy(net_config->dest_ip, temp_dest_ip, 4);
	printf("\n");
    }



    while (1) {
        printf("Enter Destination Port (the current is %u) or press return to accept the current: ", net_config->dest_port);
        int val = read_decimal_word();
	if ( val == -1)
	{
	  break;
	}
	net_config->dest_port = val;
    }

    // Set magic number and checksum before writing
    net_config->magic_number = CONFIG_MAGIC_NUMBER;
    net_config->checksum = calculate_checksum(net_config);

    // Write to flash
    write_config_to_flash(net_config);
}


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
uint8_t dest_ip_global[4];
uint16_t dest_port_global;


int main() {
    stdio_init_all();
    sleep_ms(3000); // Wait for the terminal to connect

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1); // Turn on LED to indicate activity

    gpio_init(CONFIG_BUTTON_PIN);
    gpio_set_dir(CONFIG_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(CONFIG_BUTTON_PIN);

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

    printf("Pico temperature sender initialized.\n");

    network_config_t net_config;
    bool config_loaded = false;

    // Check if the config button is pressed
    if (gpio_get(CONFIG_BUTTON_PIN) == 0) {
        printf("Config button pressed. Entering configuration mode.\n");
        // Initialize with a default configuration to display in the prompt
        memcpy(net_config.mac, (uint8_t[]){0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, 6);
        memcpy(net_config.ip, (uint8_t[]){192, 168, 2, 162}, 4);
        memcpy(net_config.sn, (uint8_t[]){255, 255, 255, 0}, 4);
        memcpy(net_config.gw, (uint8_t[]){192, 168, 2, 1}, 4);
        memcpy(net_config.dest_ip, (uint8_t[]){192, 168, 2, 10}, 4);
        net_config.dest_port = 16216;
        setup_network_via_console(&net_config);
        config_loaded = true;
    } else {
        // Try to load network config from flash
        config_loaded = read_config_from_flash(&net_config);
    }

    // Use either the loaded config or defaults if no config was loaded
    if (config_loaded) {
        memcpy(g_net_info.mac, net_config.mac, 6);
        memcpy(g_net_info.ip, net_config.ip, 4);
        memcpy(g_net_info.sn, net_config.sn, 4);
        memcpy(g_net_info.gw, net_config.gw, 4);
        memcpy(dest_ip_global, net_config.dest_ip, 4);
        dest_port_global = net_config.dest_port;
    } else {
        printf("Using default configuration.\n");
        memcpy(g_net_info.mac, (uint8_t[]){0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, 6);
        memcpy(g_net_info.ip, (uint8_t[]){192, 168, 2, 162}, 4);
        memcpy(g_net_info.sn, (uint8_t[]){255, 255, 255, 0}, 4);
        memcpy(g_net_info.gw, (uint8_t[]){192, 168, 2, 1}, 4);
        memcpy(dest_ip_global, (uint8_t[]){192, 168, 2, 10}, 4);
        dest_port_global = 16216;
    }

    // Set other net_info fields not stored in flash
#if _WIZCHIP_ > W5500
    // No IPv6 settings needed for this W5500 example
#else
    g_net_info.dhcp = NETINFO_STATIC;
#endif

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
    if (socket(SOCKET_NUM, Sn_MR_UDP, g_net_info.ip[3], 0) == 0) { // Using a local port from IP for simplicity
        printf("UDP socket opened on port %d.\n", g_net_info.ip[3]);
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
            printf("The temperature is %u F\n", packet_buffer[TEMPERATURE_BYTE_INDEX]);

            // Send the UDP packet
            int32_t len = sendto(SOCKET_NUM, packet_buffer, PACKET_SIZE, dest_ip_global, dest_port_global);

            if (len > 0) {
                printf("Sent UDP packet to %d.%d.%d.%d:%d\n",
                       dest_ip_global[0], dest_ip_global[1], dest_ip_global[2], dest_ip_global[3], dest_port_global);
            } else {
                printf("Failed to send UDP packet. Error: %ld\n", len);
            }
        }
    }

    // Clean up
    close(SOCKET_NUM);
    return 0;
}

