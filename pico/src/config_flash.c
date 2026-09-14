#include "config_flash.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"

#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_WRITE_TIMEOUT_MS 1000

static uint8_t pages[FLASH_SECTOR_SIZE];

bool config_flash_load(device_config_t *config) {
    return config_decode((const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET), CONFIG_RECORD_SIZE, config);
}

static void write_sector(void *length_pointer) {
    size_t length = *(size_t *)length_pointer;
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, pages, length);
}

bool config_flash_save(const device_config_t *config) {
    memset(pages, 0xFF, sizeof pages);
    size_t length = config_encode(config, pages, sizeof pages);
    if (length == 0) return false;
    size_t page_length = (length + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    // flash_safe_execute keeps interrupts (USB, Wi-Fi) away from the flash while XIP is off.
    if (flash_safe_execute(write_sector, &page_length, FLASH_WRITE_TIMEOUT_MS) != PICO_OK) return false;
    device_config_t check;
    return config_flash_load(&check) && memcmp(&check, config, sizeof check) == 0;
}
