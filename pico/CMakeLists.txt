cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w)
include(pico_sdk_import.cmake)

project(canvas_probe C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(canvas_probe src/main.c)
pico_generate_pio_header(canvas_probe ${CMAKE_CURRENT_LIST_DIR}/src/bus.pio)
target_compile_options(canvas_probe PRIVATE -Wall -Wextra)
target_link_libraries(canvas_probe pico_stdlib hardware_pio hardware_dma hardware_clocks)

pico_enable_stdio_usb(canvas_probe 1)
pico_enable_stdio_uart(canvas_probe 0)
pico_add_extra_outputs(canvas_probe)
