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

**Configurable fields:** MAC, DHCP (y/n), device IP, subnet, gateway, route-via-gateway (y/n), destination IP, destination port, time delay (seconds), packet style (0 or 1).

When DHCP is enabled, the IP/subnet/gateway prompts are skipped (these are obtained automatically from the DHCP server). The destination IP, port, time delay, and packet style must still be configured manually.

**Defaults:** IP 192.168.2.162, dest 192.168.2.10:16216, 10s interval, custom packet format, DHCP disabled, route-via-gateway disabled.

### Route Via Gateway

`route_via_gateway` is an optional workaround for networks that block direct
host-to-host (peer) traffic at layer 2 — e.g. switch port isolation / private
VLAN / "client isolation" on managed campus or enterprise networks. The symptom
is `sendto()` returning `SOCKERR_TIMEOUT` (-13, "Failed to send UDP packet")
because the WIZnet chip's ARP for the destination gets no reply, even though IP,
subnet, gateway, and DHCP are all valid.

When enabled, the firmware overrides the subnet mask applied to the chip with
`255.255.255.255` (a /32), so every destination is treated as off-subnet and the
chip ARPs **only the gateway**, letting the router relay the packet. This is
applied in `network.c` on both the static-IP path and the DHCP path (the real
DHCP-assigned subnet is still printed to the console for reference). It only
helps if the router is permitted to forward between the isolated ports; pure L2
isolation with no router relay cannot be worked around from the endpoint.

### DHCP Support

The firmware supports optional DHCP for obtaining IP, subnet, and gateway automatically. This is controlled by the `use_dhcp` field in the configuration.

- **Socket allocation:** Socket 0 for UDP data, socket 1 for DHCP
- **Startup:** When DHCP is enabled, `network_dhcp_run()` blocks until an IP is obtained (up to 5 retries). On failure, the device enters the error state.
- **Lease renewal:** `network_dhcp_maintain()` is called each time a temperature packet is sent, keeping the DHCP lease active.
- **Assigned configuration is printed** to the USB serial console (IP, subnet, gateway, DNS, lease time) when the DHCP lease is obtained.
- Uses the WIZnet DHCP library (`libraries/ioLibrary_Driver/Internet/DHCP/`) and the port timer (`port/timer/`) for the required 1-second tick.
