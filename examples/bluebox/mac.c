/*
 * mac.c
 * Runtime MAC-address source for bluebox.
 *
 * At boot we probe an optional 24AA02E48 I2C EEPROM (i2c0 on GPIO4/5) which
 * carries a factory-programmed EUI-48 in its write-protected upper block at
 * word addresses 0xFA..0xFF. The probe result is cached.
 *
 * Boards that lack the EEPROM (or boards where GPIO4/5 are claimed by other
 * peripherals, e.g. BLUEBOX_W5500 where GPIO4 is SPI RX) simply report
 * "not present" and the caller falls back to a randomly generated MAC.
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/rand.h"

#include "hardware.h"
#include "mac.h"

#if (DEVICE_BOARD_NAME != BLUEBOX_W5500)
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#define EEPROM_I2C_PORT      i2c0
#define EEPROM_I2C_BAUD_HZ   (100 * 1000)
#define EEPROM_SDA_PIN       4
#define EEPROM_SCL_PIN       5
#define EEPROM_I2C_ADDR      0x50
#define EEPROM_EUI48_OFFSET  0xFA
#endif

static bool    s_eeprom_present = false;
static uint8_t s_eeprom_mac[6];

void mac_init(void)
{
#if (DEVICE_BOARD_NAME == BLUEBOX_W5500)
    // GPIO4 is used by the WIZnet SPI on this board; the EEPROM cannot be
    // attached, so don't touch the bus.
    s_eeprom_present = false;
#else
    i2c_init(EEPROM_I2C_PORT, EEPROM_I2C_BAUD_HZ);
    gpio_set_function(EEPROM_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(EEPROM_SDA_PIN);
    gpio_pull_up(EEPROM_SCL_PIN);

    uint8_t word_addr = EEPROM_EUI48_OFFSET;
    int wret = i2c_write_blocking(EEPROM_I2C_PORT, EEPROM_I2C_ADDR,
                                  &word_addr, 1, true);
    if (wret < 0)
    {
        printf("MAC EEPROM (24AA02E48) not detected.\n");
        s_eeprom_present = false;
        return;
    }

    uint8_t mac[6];
    int rret = i2c_read_blocking(EEPROM_I2C_PORT, EEPROM_I2C_ADDR,
                                 mac, sizeof(mac), false);
    if (rret != (int)sizeof(mac))
    {
        printf("MAC EEPROM read failed (%d).\n", rret);
        s_eeprom_present = false;
        return;
    }

    // Guard against a stuck bus that happens to ACK but returns junk.
    bool all_ff = true, all_00 = true;
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] != 0xFF) all_ff = false;
        if (mac[i] != 0x00) all_00 = false;
    }
    if (all_ff || all_00)
    {
        printf("MAC EEPROM returned invalid value; ignoring.\n");
        s_eeprom_present = false;
        return;
    }

    memcpy(s_eeprom_mac, mac, 6);
    s_eeprom_present = true;
    printf("MAC EEPROM detected: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

bool mac_eeprom_available(void)
{
    return s_eeprom_present;
}

bool mac_eeprom_get(uint8_t mac_out[6])
{
    if (!s_eeprom_present)
        return false;
    memcpy(mac_out, s_eeprom_mac, 6);
    return true;
}

void mac_random_generate(uint8_t mac_out[6])
{
    uint64_t r = get_rand_64();
    mac_out[0] = (uint8_t)(r >>  0);
    mac_out[1] = (uint8_t)(r >>  8);
    mac_out[2] = (uint8_t)(r >> 16);
    mac_out[3] = (uint8_t)(r >> 24);
    mac_out[4] = (uint8_t)(r >> 32);
    mac_out[5] = (uint8_t)(r >> 40);

    // Locally administered (bit 1 set), unicast (bit 0 clear).
    mac_out[0] = (mac_out[0] & 0xFC) | 0x02;
}
