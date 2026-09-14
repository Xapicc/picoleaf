#include "ha_light.h"

#include <stdio.h>
#include <string.h>

static const char *skip_space(const char *cursor, const char *end) {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')) cursor++;
    return cursor;
}

// Returns a pointer to the value after `"key":` within [json, end), or NULL. Keys are
// matched with their quotes, so "r" does not match inside "brightness".
static const char *find_value(const char *json, const char *end, const char *key) {
    size_t key_length = strlen(key);
    for (const char *cursor = json; cursor + key_length + 2 <= end; cursor++) {
        if (cursor[0] != '"' || memcmp(cursor + 1, key, key_length) != 0 || cursor[key_length + 1] != '"') continue;
        const char *colon = skip_space(cursor + key_length + 2, end);
        if (colon < end && *colon == ':') return skip_space(colon + 1, end);
    }
    return NULL;
}

// Parses an integer 0..255 at `cursor`.
static bool parse_byte(const char *cursor, const char *end, uint8_t *value) {
    if (cursor == NULL || cursor >= end || *cursor < '0' || *cursor > '9') return false;
    unsigned number = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        number = number * 10 + (unsigned)(*cursor - '0');
        if (number > 255) return false;
        cursor++;
    }
    *value = (uint8_t)number;
    return true;
}

bool ha_light_apply_command(light_state_t *state, const char *payload, size_t length) {
    const char *end = payload + length;
    light_state_t updated = *state;
    bool recognised = false;

    const char *on_off = find_value(payload, end, "state");
    if (on_off) {
        if (end - on_off >= 4 && memcmp(on_off, "\"ON\"", 4) == 0) {
            updated.on = true;
        } else if (end - on_off >= 5 && memcmp(on_off, "\"OFF\"", 5) == 0) {
            updated.on = false;
        } else {
            return false;
        }
        recognised = true;
    }

    const char *brightness = find_value(payload, end, "brightness");
    if (brightness) {
        if (!parse_byte(brightness, end, &updated.brightness)) return false;
        recognised = true;
    }

    const char *colour = find_value(payload, end, "color");
    if (colour) {
        if (*colour != '{') return false;
        const char *colour_end = memchr(colour, '}', (size_t)(end - colour));
        if (colour_end == NULL) return false;
        if (!parse_byte(find_value(colour, colour_end, "r"), colour_end, &updated.red) ||
            !parse_byte(find_value(colour, colour_end, "g"), colour_end, &updated.green) ||
            !parse_byte(find_value(colour, colour_end, "b"), colour_end, &updated.blue)) {
            return false;
        }
        recognised = true;
    }

    if (!recognised) return false;
    *state = updated;
    return true;
}

static size_t checked_length(int written, size_t size) {
    return written < 0 || (size_t)written >= size ? 0 : (size_t)written;
}

size_t ha_light_state_json(const light_state_t *state, char *out, size_t size) {
    int written = snprintf(out, size,
                           "{\"state\":\"%s\",\"brightness\":%u,\"color_mode\":\"rgb\",\"color\":{\"r\":%u,\"g\":%u,"
                           "\"b\":%u}}",
                           state->on ? "ON" : "OFF", state->brightness, state->red, state->green, state->blue);
    return checked_length(written, size);
}

static uint8_t scale(uint8_t channel, uint8_t brightness) {
    return (uint8_t)((channel * brightness + 127u) / 255u);
}

void ha_light_output(const light_state_t *state, uint8_t rgbw[4]) {
    rgbw[0] = state->on ? scale(state->red, state->brightness) : 0;
    rgbw[1] = state->on ? scale(state->green, state->brightness) : 0;
    rgbw[2] = state->on ? scale(state->blue, state->brightness) : 0;
    rgbw[3] = 0;
}

size_t ha_light_discovery_json(const ha_light_discovery_t *light, char *out, size_t size) {
    int written = snprintf(out, size,
                           "{\"name\":\"%s\",\"unique_id\":\"%s\",\"schema\":\"json\",\"command_topic\":\"%s\","
                           "\"state_topic\":\"%s\",\"availability_topic\":\"%s\",\"brightness\":true,"
                           "\"supported_color_modes\":[\"rgb\"],\"device\":{\"identifiers\":[\"%s\"],"
                           "\"name\":\"Nanoleaf Canvas\",\"manufacturer\":\"FckAhLeaf\","
                           "\"model\":\"Pico W panel controller\",\"sw_version\":\"%s\"}}",
                           light->name, light->unique_id, light->command_topic, light->state_topic,
                           light->availability_topic, light->device_id, light->firmware_version);
    return checked_length(written, size);
}
