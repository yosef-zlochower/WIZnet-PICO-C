# Bluebox Code Review

## Overview

The bluebox is a temperature monitoring device built on the WIZnet W55RP20
(an integrated RP2040 + W5500 Ethernet chip). It reads temperature from a
DS18B20 one-wire sensor and sends periodic UDP packets containing the
temperature reading to a configurable destination host. An alternate build
target supports a standalone RP2040 paired with an external W5500 module
(BLUEBOX_W5500).

The code lives under `examples/bluebox/` within the WIZnet-PICO-C SDK tree.
Only the bluebox subdirectory is built (all other example subdirectories are
commented out in `examples/CMakeLists.txt`).

---

## File Inventory

| File | Purpose |
|------|---------|
| `examples/bluebox/main.c` | Entry point, initialization, core 0 main loop |
| `examples/bluebox/config.h` / `config.c` | Network configuration: flash storage, serial console UI |
| `examples/bluebox/ds18b20.h` / `ds18b20.c` | One-wire protocol, DS18B20 temperature reading, core 1 loop |
| `examples/bluebox/network.h` / `network.c` | WIZnet chip setup, SPI callbacks, UDP socket operations |
| `examples/bluebox/globals.h` | Packet templates (new and old style), shared extern declarations |
| `examples/bluebox/hardware.h` | GPIO pin assignments (board-specific via preprocessor) |
| `examples/bluebox/led_state.h` / `led_state.c` | LED blink state machine (NORMAL / ERROR / CONFIG) |
| `examples/bluebox/CMakeLists.txt` | Build target definition and library linkage |

Supporting files outside `examples/bluebox/`:

| File | Relevance |
|------|-----------|
| `CMakeLists.txt` (top-level) | Board selection (`BOARD_NAME`), chip defines, SDK paths |
| `examples/CMakeLists.txt` | Only `bluebox` subdirectory is enabled |
| `port/board_list.h` | Numeric board IDs: `BLUEBOX_W5500 = 10`, `BLUEBOX_W55RP20 = 11` |
| `port/ioLibrary_Driver/inc/wizchip_spi.h` | SPI/PIO pin definitions per board |
| `port/ioLibrary_Driver/src/wizchip_spi.c` | Low-level WIZnet SPI/PIO initialization |
| `port/CMakeLists.txt` | Conditional PIO build for W55RP20 boards |

---

## Architecture

### Dual-Core Design

- **Core 0** (main): Handles initialization, network setup, and the UDP send
  loop. Blocks on `multicore_fifo_pop_blocking()` waiting for core 1 to signal
  that a new temperature reading is ready.
- **Core 1** (`ds18b20_core1_entry`): Runs the temperature read loop. After
  each successful read, it copies the packet pattern into `packet_buffer`,
  inserts the temperature byte, and pushes `1` to the multicore FIFO to wake
  core 0.

### Startup Flow (main.c)

```
stdio_init_all()
  -> GPIO init (LED, config button)
  -> Check config button
     -> If pressed: load flash config (or defaults), enter console config mode
     -> If not pressed: load flash config, or fall back to hardcoded defaults
  -> Select packet style (new 17-byte or old 198-byte SNMP trap)
  -> initialize_ds18b20()
  -> network_open_socket()
  -> multicore_launch_core1(ds18b20_core1_entry)
  -> Main loop: wait for FIFO -> print temp -> send UDP -> repeat
```

### Data Flow

```
[DS18B20 sensor] --(one-wire)--> [Core 1: ds18b20_read_temp()]
   -> temperature inserted into packet_buffer at temperature_byte_index
   -> multicore FIFO signal (1)
   -> [Core 0: network_send_packet()] --(UDP)--> [destination host]
```

---

## Hardware Configuration

Two board variants are supported, selected at compile time via `BOARD_NAME`
in the top-level `CMakeLists.txt`.

### BLUEBOX_W55RP20 (current default)

| Function | GPIO |
|----------|------|
| LED | 3 |
| Config Button | 1 |
| One-Wire (DS18B20) | 2 |
| WIZnet SPI SCK | 21 |
| WIZnet SPI MOSI | 23 |
| WIZnet SPI MISO | 22 |
| WIZnet CS | 20 |
| WIZnet RST | 25 |
| WIZnet INT | 24 |

Uses PIO-based SPI (defined via `USE_PIO` in `wizchip_spi.h`).

### BLUEBOX_W5500 (standalone W5500 module)

| Function | GPIO |
|----------|------|
| LED | 29 |
| Config Button | 28 |
| One-Wire (DS18B20) | 6 |
| WIZnet SPI SCK | 2 |
| WIZnet SPI MOSI | 3 |
| WIZnet SPI MISO | 4 |
| WIZnet CS | 1 |
| WIZnet RST | 0 |
| WIZnet INT | 7 |

Uses hardware SPI (spi0) at 8 MHz.

---

## Network Configuration Subsystem (config.c)

### Flash Storage

- Offset: `FLASH_TARGET_OFFSET = 0x100000` (1 MB into flash)
- Validation: magic number `0x9E7F6B3D` + additive checksum
- Struct `network_config_t` stores: MAC, IP, subnet, gateway, destination
  IP, destination port, time delay (seconds between readings), packet style

### Console Configuration Mode

Triggered by holding the config button (GPIO pulled up; active low) at boot.
The serial console (USB CDC) presents an interactive menu to configure each
network parameter. Each field shows the current value and accepts new input
or Enter to keep the current value. Input is validated and confirmed with
y/n before accepting.

Configurable fields:
- MAC address (hex, colon-separated)
- IP address, subnet mask, gateway, destination IP (dotted decimal)
- Destination port (0-65535)
- Time delay between readings in seconds (minimum 1)
- Packet style: 0 = new (17-byte), 1 = old SNMP trap (198-byte)

### Default Configuration

If no valid flash config exists and the config button is not pressed:
- MAC: `00:08:DC:12:34:56`
- IP: `192.168.2.162`
- Subnet: `255.255.255.0`
- Gateway: `192.168.2.1`
- Destination: `192.168.2.10:16216`
- Time delay: 10 seconds
- Packet style: 0 (new)

---

## DS18B20 Temperature Sensor (ds18b20.c)

### One-Wire Protocol Implementation

All bit-banged on a single GPIO:
- `one_wire_reset()`: 480us low pulse, then read presence (70us after release)
- `one_wire_write_bit()` / `one_wire_read_bit()`: standard Dallas timing
- `one_wire_write_byte()` / `one_wire_read_byte()`: LSB-first

### Temperature Reading

`ds18b20_read_temp()`:
1. Reset + skip ROM (`0xCC`) + convert T (`0x44`)
2. Wait 750ms for conversion
3. Reset + skip ROM + read scratchpad (`0xBE`)
4. Read 9 bytes of scratchpad
5. Convert raw 12-bit value to Celsius, then to Fahrenheit (rounded)
6. Validate range 0-212 F; return `DS18B20_ERROR` (-128) on failure

### Sensor Fault Handling

`check_for_ds18b20()` is called before issuing commands. If the sensor is
not detected, the LED enters ERROR state (fast blink) and the code
busy-waits polling every 5 seconds until the sensor reappears. If the sensor
disconnects mid-read, DS18B20_ERROR is returned and no packet is sent for
that cycle.

### Core 1 Loop

```c
for (;;) {
    LED off  // indicates "reading"
    read temperature
    if valid: copy pattern -> insert temp byte -> FIFO push(1)
    LED on   // indicates "idle"
    sleep(time_delay - 759ms)  // 759ms accounts for conversion time
}
```

---

## Packet Formats (globals.h)

### New Style (packet_style == 0)

- Size: 17 bytes (`PACKET_SIZE_NEW`)
- Temperature byte index: 16 (last byte)
- First 16 bytes are a fixed magic pattern / identifier
- Temperature occupies the final byte as a raw Fahrenheit value (0-255)

### Old Style / SNMP Trap (packet_style == 1)

- Size: 198 bytes (`PACKET_SIZE_OLD`)
- Temperature byte index: 175
- Formatted as an SNMP v1 trap PDU:
  - Community string: "public"
  - Enterprise OID: `1.3.6.1.4.1.14453.1.1`
  - Contains varbinds with sysDescr "SH2 (4.3)" and "Temperature Sensor 1"
  - Temperature is encoded as an ASN.1 integer at byte offset 175

---

## LED State Machine (led_state.c)

Three states managed via a repeating timer:

| State | Blink Rate | Meaning |
|-------|-----------|---------|
| NORMAL | LED steady (no timer) | Normal operation |
| ERROR | 125ms toggle (8 Hz) | Sensor disconnected or network init |
| CONFIG | 250ms toggle (4 Hz) | Console configuration mode active |

State transitions are guarded: entering ERROR or CONFIG from a non-NORMAL
state prints a warning. Attempting to leave a state you're not in triggers
a permanent ERROR halt (infinite loop). The `network_setup()` function
uses enter/leave_error_state around WIZnet initialization, so the LED
blinks during network bringup.

---

## Network Layer (network.c)

### Board-Conditional Initialization

- **BLUEBOX_W5500**: Explicit hardware SPI init (spi0 @ 8 MHz), manual GPIO
  setup for CS/RST, hardware reset pulse, register SPI byte read/write
  callbacks with WIZnet library.
- **BLUEBOX_W55RP20**: Delegates to `wizchip_spi_initialize()`,
  `wizchip_cris_initialize()`, and `wizchip_reset()` from the port layer
  (PIO-based SPI).

Both paths then call:
- `wizchip_initialize()` -- WIZnet chip-level init
- `network_initialize(net_info)` -- set MAC/IP/subnet/gateway
- `print_network_information(net_info)` -- diagnostic output

### Socket Operations

- Single socket (`SOCKET_NUM = 0`) in UDP mode
- `network_open_socket(port)`: opens UDP socket on the given local port
- `network_send_packet()`: wraps `sendto()` from the WIZnet socket API
- `network_close_socket()`: wraps `close()` (never actually reached in
  normal operation since main loop is infinite)

---

## Build System

### Top-Level CMakeLists.txt

- `BOARD_NAME` selects the target board (currently `BLUEBOX_W55RP20`)
- Sets `_WIZCHIP_=W5500` and `DEVICE_BOARD_NAME` preprocessor defines
- SPI clock: 43 MHz (`WIZCHIP_SPI_SCLK_SPEED`)
- Pulls in Pico SDK, WIZnet library, mbedTLS (though mbedTLS add_subdirectory
  is commented out)

### Bluebox CMakeLists.txt

Sources: `main.c`, `config.c`, `ds18b20.c`, `network.c`, `led_state.c`

Linked libraries:
- `pico_stdlib`, `pico_stdio_usb`, `pico_multicore`
- `hardware_spi`, `hardware_dma`
- `ETHERNET_FILES`, `IOLIBRARY_FILES` (WIZnet SDK)

stdio: USB enabled, UART disabled.

---

## Key Global Variables (globals.h + main.c)

| Variable | Type | Purpose |
|----------|------|---------|
| `packet_pattern[]` | `uint8_t[PACKET_SIZE_MAX]` | Template packet copied before each send |
| `packet_buffer[]` | `uint8_t[PACKET_SIZE_MAX]` | Working buffer with temperature inserted |
| `packet_size` | `uint16_t` | Active packet size (17 or 198) |
| `temperature_byte_index` | `uint16_t` | Offset where temperature is placed in packet |
| `dest_ip_global[]` | `uint8_t[4]` | Destination IP address |
| `dest_port_global` | `uint16_t` | Destination UDP port |
| `time_delay_global` | `uint16_t` | Seconds between readings |
| `packet_style` | `uint8_t` | 0 = new, 1 = old SNMP trap |

These are declared `extern` in `globals.h` and defined in `main.c`. They are
shared between core 0 and core 1 (core 1 reads the pattern/size/index, core 0
reads the buffer after FIFO signal).

---

## Notes for Future Work

### Adding Hardware Debugging Routines

Key integration points for hardware debug code:

1. **GPIO state inspection**: `hardware.h` defines all pin assignments.
   Debug routines could read/report the state of WIZnet control pins
   (CS, RST, INT), the one-wire pin, LED, and config button.

2. **W5500 register reads**: The WIZnet library provides register access
   functions. Debug routines could dump PHY status, link state, socket
   status registers, and TX/RX buffer pointers via `wizchip_conf.h` APIs
   (e.g., `getPHYCFGR()`, `getSn_SR()`, `getSn_TX_FSR()`).

3. **One-wire diagnostics**: The one-wire functions in `ds18b20.c` are
   static. To expose them for debugging, either make select functions
   non-static or add dedicated debug wrappers that test reset/presence
   and raw scratchpad reads.

4. **Network diagnostics**: `network.c` could expose functions to read
   back the configured IP/MAC from the W5500 registers and compare
   against the intended configuration, check link status, and report
   socket state.

5. **Build integration**: New debug source files should be added to
   `examples/bluebox/CMakeLists.txt` in the `add_executable` and
   potentially linked against additional `hardware_*` libraries as needed.

6. **Console interface**: The USB serial console (already enabled via
   `pico_stdio_usb`) is the natural channel for debug output. A debug
   command parser could be added to core 0's main loop or triggered by
   a second button/GPIO.

7. **LED state**: `led_state.c` provides visual feedback. Debug modes
   could add additional blink patterns or use a second LED if available.
