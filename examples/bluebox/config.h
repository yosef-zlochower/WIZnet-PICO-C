#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

// Data structure to hold network configuration
typedef struct {
  uint8_t mac[6];
  uint8_t ip[4];
  uint8_t sn[4];
  uint8_t gw[4];
  uint8_t dest_ip[4];
  uint16_t dest_port;
  uint16_t time_delay;
  uint32_t checksum;
  uint32_t magic_number;
} network_config_t;

// A unique number to identify valid configuration data in flash
#define CONFIG_MAGIC_NUMBER 0x9E7F6B3D

// Function declarations
uint32_t calculate_checksum(const network_config_t *config);
bool read_config_from_flash(network_config_t *config);
void write_config_to_flash(const network_config_t *config);
void setup_network_via_console(network_config_t *net_config);

#endif
