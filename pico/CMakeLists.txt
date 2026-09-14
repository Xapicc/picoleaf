cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w)
include(pico_sdk_import.cmake)

project(canvas_probe C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(canvas_probe
    src/main.c
    src/controller.c
    src/uart_decode.c
    src/ha_light.c
    src/effects.c
    src/layout.c
    src/renderer.c
    src/connection_watch.c
    src/home_assistant.c
    src/config_record.c
    src/config_flash.c
    src/net.c
)
target_include_directories(canvas_probe PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)  # lwipopts.h
pico_generate_pio_header(canvas_probe ${CMAKE_CURRENT_LIST_DIR}/src/bus.pio)
target_compile_options(canvas_probe PRIVATE -Wall -Wextra)
# The 2 KB default main stack is too tight for MQTT publishing plus snprintf; 4 KB is the most the
# default linker script's stack region holds. Large message buffers are static instead.
target_compile_definitions(canvas_probe PRIVATE PICO_STACK_SIZE=0x1000)
target_link_libraries(canvas_probe
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_flash
    hardware_watchdog
    pico_flash
    pico_unique_id
    pico_cyw43_arch_lwip_poll
    pico_lwip_mqtt
)

pico_enable_stdio_usb(canvas_probe 1)
pico_enable_stdio_uart(canvas_probe 0)
pico_add_extra_outputs(canvas_probe)
