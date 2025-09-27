#include "board_list.h"
#if (DEVICE_BOARD_NAME == BLUEBOX_W5500)
// --- Hardware Pin Configurations ---
#define LED_PIN 29
#define CONFIG_BUTTON_PIN 28

#define WIZ_SPI_PORT spi0
#define WIZ_SPI_RX_PIN 4
#define WIZ_SPI_SCK_PIN 2
#define WIZ_SPI_TX_PIN 3
#define WIZ_CS_PIN 1
#define WIZ_RST_PIN 0

#define ONE_WIRE_PIN 6

#elif (DEVICE_BOARD_NAME == BLUEBOX_W55RP20 || DEVICE_BOARD_NAME == W55RP20_EVB_PICO)

// --- Hardware Pin Configurations ---
#define LED_PIN 3
#define CONFIG_BUTTON_PIN 1
#define WIZ_SPI_PORT spi0
#define WIZ_SPI_RX_PIN 23
#define WIZ_SPI_SCK_PIN 21
#define WIZ_SPI_TX_PIN 23
#define WIZ_CS_PIN 20
#define WIZ_RST_PIN 25

#define ONE_WIRE_PIN 2
#endif
