#ifndef _NETWORK_H_
#define _NETWORK_H_

#include "wizchip_conf.h"
#include "config.h"
#define SOCKET_NUM 0

// Initialize network hardware and software
void network_setup(network_config_t config);

// Open a UDP socket
int network_open_socket(uint16_t local_port);

// Send a UDP packet
int32_t network_send_packet(uint8_t *packet, uint16_t size, uint8_t *dest_ip, uint16_t dest_port);

void network_close_socket(void);
#endif

