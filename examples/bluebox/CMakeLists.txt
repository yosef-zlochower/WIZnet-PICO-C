# CMake configuration for the W55RP20-EVB-PICO UDP Sender example.
# This file defines the build target and links the necessary libraries
# from the WIZnet-PICO-C repository.

set(TARGET_NAME bluebox)

# Add the main source file to the executable
add_executable(${TARGET_NAME}
    bluebox.c
)

# Link the required libraries for Pico and WIZnet
target_link_libraries(${TARGET_NAME} PRIVATE
    pico_stdlib
    hardware_spi
    hardware_dma
    ETHERNET_FILES
    IOLIBRARY_FILES
)

# Enable USB and disable UART for standard I/O
pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

# Add extra output files for the build (UF2, ELF, etc.)
pico_add_extra_outputs(${TARGET_NAME})

