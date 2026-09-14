#pragma once

// Device settings (Wi-Fi and MQTT) and their on-flash record format. Pure C so
// it runs in the host tests; flash access lives in config_flash.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_password[64];
    char mqtt_host[64];  // IPv4 address, e.g. 192.168.1.10
    uint16_t mqtt_port;
    char mqtt_user[65];
    char mqtt_password[65];
    // Clockwise rotation of the wall relative to the first square's frame: 0, 90, 180 or 270.
    uint16_t layout_rotation;
} device_config_t;

// Record: magic, version, payload length, payload, CRC-32 over everything before it.
#define CONFIG_RECORD_SIZE (4 + 2 + 2 + sizeof(device_config_t) + 4)

void config_defaults(device_config_t *config);

// Returns the number of bytes written, or 0 if `size` < CONFIG_RECORD_SIZE.
size_t config_encode(const device_config_t *config, uint8_t *record, size_t size);

// Returns false (and leaves `config` untouched) for erased flash, an unknown version or a bad CRC.
// Version 1 records (before layout_rotation) are read with layout_rotation = 0.
bool config_decode(const uint8_t *record, size_t size, device_config_t *config);

// Sets one field by name: wifi_ssid, wifi_password, mqtt_host, mqtt_port, mqtt_user, mqtt_password,
// layout_rotation. Returns false for an unknown key, a value that doesn't fit, a port outside 1..65535
// or a rotation other than 0/90/180/270.
bool config_set_field(device_config_t *config, const char *key, const char *value);
