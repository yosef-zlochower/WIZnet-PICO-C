#ifndef _NETWORK_H_
#define _NETWORK_H_

#include <stdbool.h>

#include "config.h"
#include "wizchip_conf.h"
#define SOCKET_NUM 0
#define SOCKET_DHCP 1

// Initialize network hardware and software
void network_setup(network_config_t config);

// Run DHCP to obtain IP address. Returns true on success.
bool network_dhcp_run(void);

// Call periodically for DHCP lease renewal (no-op if DHCP not active)
void network_dhcp_maintain(void);

// Open a UDP socket
int network_open_socket(uint16_t local_port);

// Send a UDP packet
int32_t network_send_packet(uint8_t *packet, uint16_t size, uint8_t *dest_ip,
                            uint16_t dest_port);

void network_close_socket(void);
#endif
