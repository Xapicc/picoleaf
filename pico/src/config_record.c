#include "config_record.h"

#include <stdlib.h>
#include <string.h>

#define CONFIG_MAGIC 0x53564E43u  // "CNVS"
#define CONFIG_VERSION 1u

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

bool config_decode(const uint8_t *record, size_t size, device_config_t *config) {
    if (size < CONFIG_RECORD_SIZE) return false;
    if (get_u32(record) != CONFIG_MAGIC) return false;
    if ((record[4] | record[5] << 8) != CONFIG_VERSION) return false;
    if ((record[6] | record[7] << 8) != sizeof *config) return false;
    if (get_u32(record + 8 + sizeof *config) != crc32(record, 8 + sizeof *config)) return false;
    device_config_t decoded;
    memcpy(&decoded, record + 8, sizeof decoded);
    // Never hand out unterminated strings, whatever the flash contains.
    decoded.wifi_ssid[sizeof decoded.wifi_ssid - 1] = '\0';
    decoded.wifi_password[sizeof decoded.wifi_password - 1] = '\0';
    decoded.mqtt_host[sizeof decoded.mqtt_host - 1] = '\0';
    decoded.mqtt_user[sizeof decoded.mqtt_user - 1] = '\0';
    decoded.mqtt_password[sizeof decoded.mqtt_password - 1] = '\0';
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
    if (strcmp(key, "mqtt_port") == 0) {
        char *end;
        unsigned long port = strtoul(value, &end, 10);
        if (*value == '\0' || *end != '\0' || port == 0 || port > 65535) return false;
        config->mqtt_port = (uint16_t)port;
        return true;
    }
    return false;
}
