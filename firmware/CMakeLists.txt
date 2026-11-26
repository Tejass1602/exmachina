cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(exmachina_probe)

# Board target: Raspberry Pi Pico 2 W (RP2350A)
# Use the rpi_pico2 board definition with rp2350a/m33/w variant
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# Source files
target_sources(app PRIVATE
    src/main.c
)

# Link with shared protocol header
zephyr_include_directories(../shared)