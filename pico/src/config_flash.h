#pragma once

#include <stdbool.h>

#include "config_record.h"

// Settings live in the last flash sector, which firmware uploads don't touch.

// Returns false if no valid settings are stored (`config` is left unchanged).
bool config_flash_load(device_config_t *config);

// Returns false if the flash couldn't be written safely.
bool config_flash_save(const device_config_t *config);
