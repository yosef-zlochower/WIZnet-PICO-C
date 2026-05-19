# CLAUDE.md

## Project Overview

**WIZnet-PICO-C** (v2.2.0) is an embedded C Ethernet SDK for Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) microcontrollers with WIZnet ethernet chips (W5100S, W5500, W6100, W6300). This is a fork; the active development focus is the **bluebox** example.

## Repository Structure

```
WIZnet-PICO-C/
├── libraries/                  # External submodules
│   ├── ioLibrary_Driver/       # WIZnet chip drivers + Internet protocols (DHCP, DNS, MQTT, SNMP, etc.)
│   ├── mbedtls/                # TLS/SSL
│   └── pico-sdk/               # Raspberry Pi Pico SDK
├── port/                       # MCU-specific porting layer
│   ├── ioLibrary_Driver/       # SPI/QSPI interface to WIZnet chips
│   ├── timer/                  # Timer implementation
│   ├── board/can/              # CAN bus driver (PIO-based)
│   └── board_list.h            # Board identifier defines
├── examples/                   # Application examples (only bluebox is enabled)
│   └── bluebox/                # Active: temperature monitoring firmware
└── CMakeLists.txt              # Root build config (board selection, chip config)
```

## Build System

- **CMake 3.12+**, C11, targeting ARM (RP2040/RP2350)
- Board selection in top-level `CMakeLists.txt` line ~11: `set(BOARD_NAME BLUEBOX_W55RP20)`
- Only the bluebox example is currently enabled in `examples/CMakeLists.txt`
- SPI clock: 43 MHz (configurable via `_WIZCHIP_SPI_SCLK_SPEED`)

### Building

```bash
cd build
cmake ..
make bluebox
```

The output is `build/examples/bluebox/bluebox.uf2` for flashing.

## Bluebox Firmware

The bluebox is a dual-core temperature monitoring device that periodically reads a DS18B20 sensor and sends the temperature over UDP to a configured host.

### Source Files (`examples/bluebox/`)

| File | Purpose |
|------|---------|
| `main.c` | Entry point: config loading, network init, core1 launch, UDP send loop |
| `config.c/h` | Network config: flash storage (at 1MB offset), interactive console setup |
| `ds18b20.c/h` | DS18B20 one-wire temperature sensor driver (bit-bang GPIO) |
| `network.c/h` | WIZnet initialization, DHCP client, UDP socket open/send/close |
| `led_state.c/h` | LED state machine: NORMAL (steady), ERROR (fast blink), CONFIG (medium blink) |
| `hardware.h` | Board-specific GPIO pin definitions (conditional on DEVICE_BOARD_NAME) |
| `globals.h` | Packet templates (SNMP trap + custom UDP), constants |
| `bluebox.c` | Older monolithic version (reference/backup) |

### Architecture

- **Core 0**: Handles network config, initializes WIZnet, opens UDP socket, blocks on FIFO waiting for core 1 to signal a temperature reading, then sends the UDP packet.
- **Core 1**: Reads DS18B20 sensor every N seconds (configurable), writes temperature into the packet buffer, signals core 0 via multicore FIFO.

### Two Hardware Variants

| | BLUEBOX_W5500 | BLUEBOX_W55RP20 |
|---|---|---|
| Hardware | Separate RP2040 + W5500 chip | Integrated W55RP20 module |
| One-wire pin | GPIO6 | GPIO2 |
| LED pin | GPIO29 | GPIO3 |
| Config button | GPIO28 | GPIO1 |
| SPI init | Manual GPIO-based | Platform abstraction (`wizchip_spi_initialize()`) |

Pin mappings are in `examples/bluebox/hardware.h`.

### Packet Formats

**Style 0 (custom UDP, 17 bytes):** 16-byte UUID header + 1 byte temperature (Fahrenheit).

**Style 1 (SNMP v1 trap, 198 bytes):** Full BER-encoded SNMP trap with enterprise OID `.1.3.6.1.4.1.58741.1.1.0`, temperature at byte offset 175.

### Configuration

Stored in flash at 1MB offset with magic number + XOR checksum validation. Enter config mode by holding the config button at boot (interactive USB serial console).

**Configurable fields:** MAC, DHCP (y/n), device IP, subnet, gateway, destination IP, destination port, time delay (seconds), packet style (0 or 1).

When DHCP is enabled, the IP/subnet/gateway prompts are skipped (these are obtained automatically from the DHCP server). The destination IP, port, time delay, and packet style must still be configured manually.

**Defaults:** IP 192.168.2.162, dest 192.168.2.10:16216, 10s interval, custom packet format, DHCP disabled.

### Route Via Gateway (removed)

A `route_via_gateway` option once existed (commit 4c7e752): it forced a
`255.255.255.255` /32 mask so the chip ARPed only the gateway, as a workaround
for suspected L2 client isolation causing `sendto()` `SOCKERR_TIMEOUT`.

It was **removed** after the real root cause was found and fixed: the DHCP
MAC-corruption bug (commit 87d9b3f, "Preserve chip MAC across DHCP assign/renew
callback") wrote uninitialized stack garbage into the chip's MAC register on
every DHCP assign/renew, so all UDP traffic went out with a bogus, changing
source MAC — producing the same `SOCKERR_TIMEOUT` / no-ARP-reply symptom on a
managed network. With that fixed and DHCP on / route-via-gateway off, packets
sent reliably to an on-subnet peer with no gateway relay, confirming there was
no genuine L2 isolation and the workaround was a misdiagnosis. Do not
reintroduce it without first ruling out MAC/ARP-level faults.

### DHCP Support

The firmware supports optional DHCP for obtaining IP, subnet, and gateway automatically. This is controlled by the `use_dhcp` field in the configuration.

- **Socket allocation:** Socket 0 for UDP data, socket 1 for DHCP
- **Startup:** When DHCP is enabled, `network_dhcp_run()` blocks until an IP is obtained (up to 5 retries). On failure, the device enters the error state.
- **Lease renewal:** `network_dhcp_maintain()` is called each time a temperature packet is sent, keeping the DHCP lease active.
- **Assigned configuration is printed** to the USB serial console (IP, subnet, gateway, DNS, lease time) when the DHCP lease is obtained.
- Uses the WIZnet DHCP library (`libraries/ioLibrary_Driver/Internet/DHCP/`) and the port timer (`port/timer/`) for the required 1-second tick.
