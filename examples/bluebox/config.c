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

#include "config.h"
#include "hardware.h"

static volatile bool led_on = false;
// This is the callback function that the timer will execute.
static bool repeating_timer_callback(struct repeating_timer *t) {
  // Toggle the state of the LED pin.
  if (led_on) {
    gpio_put(LED_PIN, 0); // Turn LED off
    led_on = false;
  } else {
    gpio_put(LED_PIN, 1); // Turn LED on
    led_on = true;
  }
  // Return true to continue the timer.
  return true;
}

// Flash memory configuration for storing network settings
#define FLASH_TARGET_OFFSET (1024 * 1024)
const uint8_t *flash_target_contents =
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

// --- Internal Helper Functions ---

// Custom function to read a single hexadecimal byte with validation and echo
static int read_hex_byte() {
  int i = 0;
  char c;
  int tval = 0;
  while (i < 2) {
    c = getchar();
    if (c == '\r' || c == '\n' || c == ':' || c == EOF) {
      if (i > 0)
        break;
      else
        return -2; // Indicates empty input
    }
    if (c == '\b' || c == 0x7F) { // Handle backspace
      if (i > 0) {
        i--;
        tval /= 16;
        printf("\b \b");
      }
      continue;
    }
    c = toupper(c);
    if (c >= '0' && c <= '9') {
      i++;
      tval = 16 * tval + c - '0';
      printf("%c", c);
    } else if (c >= 'A' && c <= 'F') {
      i++;
      tval = 16 * tval + 10 + c - 'A';
      printf("%c", c);
    }
  }
  return tval;
}

// Custom function to read a single decimal byte with validation and echo
static int read_decimal_byte() {
  int i = 0;
  char c;
  int tval = 0;
  while (i < 3) {
    c = getchar();
    if (c == '\r' || c == '\n' || c == '.' || c == EOF) {
      if (i > 0)
        break;
      else
        return -2; // Indicates empty input
    }
    if (c == '\b' || c == 0x7F) { // Handle backspace
      if (i > 0) {
        i--;
        tval /= 10;
        printf("\b \b");
      }
      continue;
    }
    if (c >= '0' && c <= '9') {
      int nval = 10 * tval + c - '0';
      if (nval > 255) {
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
static int read_decimal_word() {
  int i = 0;
  char c;
  int tval = 0;
  while (i < 5) {
    c = getchar();
    if (c == '\r' || c == '\n' || c == EOF) {
      if (i > 0)
        break;
      else
        return -2; // Indicates empty input
    }
    if (c == '\b' || c == 0x7F) { // Handle backspace
      if (i > 0) {
        i--;
        tval /= 10;
        printf("\b \b");
      }
      continue;
    }
    if (c >= '0' && c <= '9') {
      int nval = 10 * tval + c - '0';
      if (nval > 65535) {
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
static char get_confirmation() {
  char c = getchar();
  putchar(c);
  printf("\n");
  return c;
}

// --- Public API Functions ---

// Calculate a simple checksum
uint32_t calculate_checksum(const network_config_t *config) {
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
  return sum;
}

// Function to read the configuration from flash
bool read_config_from_flash(network_config_t *config) {
  memcpy(config, flash_target_contents, sizeof(network_config_t));
  if (config->magic_number == CONFIG_MAGIC_NUMBER &&
      config->checksum == calculate_checksum(config)) {
    printf("Successfully loaded network configuration from flash.\n");
    return true;
  }
  printf("No valid network configuration found in flash. Using defaults.\n");
  return false;
}

// Function to write the configuration to flash
void write_config_to_flash(const network_config_t *config) {
  uint8_t buffer[FLASH_SECTOR_SIZE];
  memcpy(buffer, config, sizeof(network_config_t));

  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_SECTOR_SIZE);
  restore_interrupts(interrupts);

  printf("Network configuration saved to flash.\n");
}

// Function to set up network config via console with validation
void setup_network_via_console(network_config_t *net_config) {
  uint8_t temp_mac[6];
  uint8_t temp_ip[4];
  uint8_t temp_sn[4];
  uint8_t temp_gw[4];
  uint8_t temp_dest_ip[4];
  int val;
  char confirm;

  struct repeating_timer timer;
  gpio_put(LED_PIN, 0);

  add_repeating_timer_us(-125000, repeating_timer_callback, NULL, &timer);

  do {
    printf("\n Press Enter to start configuration\n");
    char c = getchar_timeout_us(5000000);
    if (c == '\n' || c == '\r') {
      break;
    }
  } while (1);
  cancel_repeating_timer(&timer);
  gpio_put(LED_PIN, 0);

  printf("\n--- Entering Network Configuration Mode ---\n");

  do {
    printf("Enter new MAC address (current: %02X:%02X:%02X:%02X:%02X:%02X)\n",
           net_config->mac[0], net_config->mac[1], net_config->mac[2],
           net_config->mac[3], net_config->mac[4], net_config->mac[5]);
    printf("or press return to accept current: ");
    val = read_hex_byte();
    if (val == -2)
      break; // Accept current value
    temp_mac[0] = val;
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

    if (temp_mac[0] != -2 && temp_mac[1] != -2 && temp_mac[2] != -2 &&
        temp_mac[3] != -2 && temp_mac[4] != -2 && temp_mac[5] != -2) {
      printf(
          "\nNew MAC: %02X:%02X:%02X:%02X:%02X:%02X. Is this correct? (y/n): ",
          temp_mac[0], temp_mac[1], temp_mac[2], temp_mac[3], temp_mac[4],
          temp_mac[5]);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        memcpy(net_config->mac, temp_mac, 6);
        break;
      }
    } else {
      printf(
          "\nInvalid MAC address. Please use the format XX:XX:XX:XX:XX:XX.\n");
    }
  } while (1);

  do {
    printf("\nEnter new IP address (current: %d.%d.%d.%d)\n", net_config->ip[0],
           net_config->ip[1], net_config->ip[2], net_config->ip[3]);
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
        temp_ip[3] != -2) {
      printf("\nNew IP: %d.%d.%d.%d. Is this correct? (y/n): ", temp_ip[0],
             temp_ip[1], temp_ip[2], temp_ip[3]);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        memcpy(net_config->ip, temp_ip, 4);
        break;
      }
    } else {
      printf("\nInvalid IP address. Please use the format X.X.X.X.\n");
    }
  } while (1);

  do {
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
        temp_sn[3] != -2) {
      printf("\nNew Subnet: %d.%d.%d.%d. Is this correct? (y/n): ", temp_sn[0],
             temp_sn[1], temp_sn[2], temp_sn[3]);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        memcpy(net_config->sn, temp_sn, 4);
        break;
      }
    } else {
      printf("\nInvalid Subnet Mask. Please use the format X.X.X.X.\n");
    }
  } while (1);

  do {
    printf("\nEnter new Gateway (current: %d.%d.%d.%d)\n", net_config->gw[0],
           net_config->gw[1], net_config->gw[2], net_config->gw[3]);
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
        temp_gw[3] != -2) {
      printf("\nNew Gateway: %d.%d.%d.%d. Is this correct? (y/n): ", temp_gw[0],
             temp_gw[1], temp_gw[2], temp_gw[3]);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        memcpy(net_config->gw, temp_gw, 4);
        break;
      }
    } else {
      printf("\nInvalid Gateway. Please use the format X.X.X.X.\n");
    }
  } while (1);

  do {
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
        temp_dest_ip[2] != -2 && temp_dest_ip[3] != -2) {
      printf("\nNew Destination IP: %d.%d.%d.%d. Is this correct? (y/n): ",
             temp_dest_ip[0], temp_dest_ip[1], temp_dest_ip[2],
             temp_dest_ip[3]);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        memcpy(net_config->dest_ip, temp_dest_ip, 4);
        break;
      }
    } else {
      printf("\nInvalid Destination IP. Please use the format X.X.X.X.\n");
    }
  } while (1);

  do {
    printf("\nEnter new Destination Port (current: %u)\n",
           net_config->dest_port);
    printf("or press return to accept current: ");
    val = read_decimal_word();
    if (val == -2)
      break;
    if (val != -1) {
      printf("\nNew Destination Port: %u. Is this correct? (y/n): ", val);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        net_config->dest_port = val;
        break;
      }
    } else {
      printf("\nInvalid Destination Port. Please enter a valid port number.\n");
    }
  } while (1);

  do {
    printf("\nEnter time delay between between temperature readings (current: %u)\n",
           net_config->time_delay);
    printf("or press return to accept current: ");
    val = read_decimal_word();
    if (val == -2)
      break;
    if (val != -1) {
      printf("\nTime delay: %u (s). Is this correct? (y/n): ", val);
      confirm = get_confirmation();
      if (confirm == 'y' || confirm == 'Y') {
        net_config->time_delay = val;
        break;
      }
    } else {
      printf("\nInvalid time delay Port. Please enter a valid time delay.\n");
    }
  } while (1);


  // Set magic number and checksum before writing
  net_config->magic_number = CONFIG_MAGIC_NUMBER;
  net_config->checksum = calculate_checksum(net_config);

  // Write to flash
  write_config_to_flash(net_config);
  gpio_put(LED_PIN, 1);
}
