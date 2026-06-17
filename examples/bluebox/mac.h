#ifndef _MAC_H_
#define _MAC_H_

#include <stdbool.h>
#include <stdint.h>

// Probe the 24AA02E48 EUI-48 EEPROM (i2c0 on GPIO4/5) once at boot and cache
// the result. Safe to call on boards without the EEPROM; absence is silently
// recorded and reported by mac_eeprom_available().
void mac_init(void);

// True if a valid EUI-48 was read from the EEPROM during mac_init().
bool mac_eeprom_available(void);

// Copy the cached EEPROM MAC into mac_out. Returns false if no EEPROM was
// detected, in which case mac_out is left untouched.
bool mac_eeprom_get(uint8_t mac_out[6]);

// Generate a random MAC address in the locally-administered, unicast range
// (first octet bits xxxxxx10).
void mac_random_generate(uint8_t mac_out[6]);

#endif
