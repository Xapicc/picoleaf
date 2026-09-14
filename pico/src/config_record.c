#include "config_record.h"

#include <stdlib.h>
#include <string.h>

#define CONFIG_MAGIC 0x53564E43u  // "CNVS"
#define CONFIG_VERSION 2u
#define CONFIG_HEADER_SIZE 8

// The version 1 payload: device_config_t before layout_rotation was added.
typedef struct {
    char wifi_ssid[33];
    char wifi_password[64];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[65];
    char mqtt_password[65];
} device_config_v1_t;

static uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

static void put_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value) {
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_u32(const uint8_t *in) {
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

void config_defaults(device_config_t *config) {
    memset(config, 0, sizeof *config);
    config->mqtt_port = 1883;
}

size_t config_encode(const device_config_t *config, uint8_t *record, size_t size) {
    if (size < CONFIG_RECORD_SIZE) return 0;
    put_u32(record, CONFIG_MAGIC);
    put_u16(record + 4, CONFIG_VERSION);
    put_u16(record + 6, (uint16_t)sizeof *config);
    memcpy(record + 8, config, sizeof *config);
    put_u32(record + 8 + sizeof *config, crc32(record, 8 + sizeof *config));
    return CONFIG_RECORD_SIZE;
}

static uint16_t get_u16(const uint8_t *in) {
    return (uint16_t)(in[0] | in[1] << 8);
}

// Checks magic, version/length pairing and CRC; returns the payload length or 0.
static size_t valid_payload_length(const uint8_t *record, size_t size) {
    if (size < CONFIG_HEADER_SIZE + 4 || get_u32(record) != CONFIG_MAGIC) return 0;
    uint16_t version = get_u16(record + 4);
    size_t length = get_u16(record + 6);
    bool known = (version == 1 && length == sizeof(device_config_v1_t)) ||
                 (version == CONFIG_VERSION && length == sizeof(device_config_t));
    if (!known || size < CONFIG_HEADER_SIZE + length + 4) return 0;
    if (get_u32(record + CONFIG_HEADER_SIZE + length) != crc32(record, CONFIG_HEADER_SIZE + length)) return 0;
    return length;
}

bool config_decode(const uint8_t *record, size_t size, device_config_t *config) {
    size_t length = valid_payload_length(record, size);
    if (length == 0) return false;
    device_config_t decoded;
    config_defaults(&decoded);
    if (length == sizeof(device_config_v1_t)) {
        device_config_v1_t old;
        memcpy(&old, record + CONFIG_HEADER_SIZE, sizeof old);
        memcpy(decoded.wifi_ssid, old.wifi_ssid, sizeof decoded.wifi_ssid);
        memcpy(decoded.wifi_password, old.wifi_password, sizeof decoded.wifi_password);
        memcpy(decoded.mqtt_host, old.mqtt_host, sizeof decoded.mqtt_host);
        decoded.mqtt_port = old.mqtt_port;
        memcpy(decoded.mqtt_user, old.mqtt_user, sizeof decoded.mqtt_user);
        memcpy(decoded.mqtt_password, old.mqtt_password, sizeof decoded.mqtt_password);
    } else {
        memcpy(&decoded, record + CONFIG_HEADER_SIZE, sizeof decoded);
    }
    // Never hand out unterminated strings or invalid values, whatever the flash contains.
    decoded.wifi_ssid[sizeof decoded.wifi_ssid - 1] = '\0';
    decoded.wifi_password[sizeof decoded.wifi_password - 1] = '\0';
    decoded.mqtt_host[sizeof decoded.mqtt_host - 1] = '\0';
    decoded.mqtt_user[sizeof decoded.mqtt_user - 1] = '\0';
    decoded.mqtt_password[sizeof decoded.mqtt_password - 1] = '\0';
    if (decoded.layout_rotation % 90 != 0 || decoded.layout_rotation >= 360) decoded.layout_rotation = 0;
    *config = decoded;
    return true;
}

static bool copy_string(char *field, size_t field_size, const char *value) {
    size_t length = strlen(value);
    if (length >= field_size) return false;
    memcpy(field, value, length + 1);
    return true;
}

bool config_set_field(device_config_t *config, const char *key, const char *value) {
    if (strcmp(key, "wifi_ssid") == 0) return copy_string(config->wifi_ssid, sizeof config->wifi_ssid, value);
    if (strcmp(key, "wifi_password") == 0) {
        return copy_string(config->wifi_password, sizeof config->wifi_password, value);
    }
    if (strcmp(key, "mqtt_host") == 0) return copy_string(config->mqtt_host, sizeof config->mqtt_host, value);
    if (strcmp(key, "mqtt_user") == 0) return copy_string(config->mqtt_user, sizeof config->mqtt_user, value);
    if (strcmp(key, "mqtt_password") == 0) {
        return copy_string(config->mqtt_password, sizeof config->mqtt_password, value);
    }
    if (strcmp(key, "layout_rotation") == 0) {
        if (strcmp(value, "0") && strcmp(value, "90") && strcmp(value, "180") && strcmp(value, "270")) return false;
        config->layout_rotation = (uint16_t)atoi(value);
        return true;
    }
    if (strcmp(key, "mqtt_port") == 0) {
        char *end;
        unsigned long port = strtoul(value, &end, 10);
        if (*value == '\0' || *end != '\0' || port == 0 || port > 65535) return false;
        config->mqtt_port = (uint16_t)port;
        return true;
    }
    return false;
}
