# CMakeLists.txt
#
# Standard Pico SDK project layout. cec_transceiver.c is NOT included here
# yet -- it doesn't exist until you've adapted it from the real pico-cec
# source (see cec_transceiver.h for the interface it must implement).
#
# Usage:
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   mkdir build && cd build
#   cmake ..
#   make -j4
#   # then copy pico-cec-bridge.uf2 to the Pico in BOOTSEL mode

cmake_minimum_required(VERSION 3.13)

# Board: Waveshare RP2040-Zero. If your pico-sdk version predates this
# board definition (added a while back, but check `ls $PICO_SDK_PATH/src/boards/include/boards/`
# if pico_sdk_init() complains it can't find waveshare_rp2040_zero.h),
# fall back to `pico` -- the silicon and pinout-via-raw-GPIO-number are
# identical either way, this define only affects default-pin convenience
# macros we don't rely on (we set all our own pins explicitly in main.c).
set(PICO_BOARD waveshare_rp2040_zero)

# Point at the submodule by default; users can override via env var or -D.
set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../pico-sdk")
include(pico_sdk_import.cmake)

project(pico_cec_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico-cec-bridge
    main.c
    cec_transceiver.c
    ws2812.c
)

target_link_libraries(pico-cec-bridge
    pico_stdlib
    hardware_i2c
    hardware_irq
    hardware_timer
    hardware_pio
)

pico_generate_pio_header(pico-cec-bridge ${CMAKE_CURRENT_SOURCE_DIR}/ws2812.pio)

# USB serial (stdio over USB CDC), no UART console -- matches the PC-side
# listener which expects a /dev/ttyACMx style device.
pico_enable_stdio_usb(pico-cec-bridge 1)
pico_enable_stdio_uart(pico-cec-bridge 0)

pico_add_extra_outputs(pico-cec-bridge)
