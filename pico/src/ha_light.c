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

    if (find_value(payload, end, "effect") || find_value(payload, end, "transition")) recognised = true;
    if (!recognised) return false;
    *state = updated;
    return true;
}

bool ha_light_command_effect(const char *payload, size_t length, char *name, size_t size) {
    const char *end = payload + length;
    const char *value = find_value(payload, end, "effect");
    if (value == NULL || *value != '"') return false;
    const char *close = memchr(value + 1, '"', (size_t)(end - value - 1));
    if (close == NULL || (size_t)(close - value - 1) >= size) return false;
    memcpy(name, value + 1, (size_t)(close - value - 1));
    name[close - value - 1] = '\0';
    return true;
}

bool ha_light_command_transition_ms(const char *payload, size_t length, uint32_t *milliseconds) {
    const char *end = payload + length;
    const char *cursor = find_value(payload, end, "transition");
    if (cursor == NULL || cursor >= end || *cursor < '0' || *cursor > '9') return false;
    uint64_t whole = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9' && whole < 100000) whole = whole * 10 + (uint64_t)(*cursor++ - '0');
    uint64_t result = whole * 1000;
    if (cursor < end && *cursor == '.') {
        uint64_t scale = 100;
        for (cursor++; cursor < end && *cursor >= '0' && *cursor <= '9'; cursor++) {
            result += (uint64_t)(*cursor - '0') * scale;
            scale /= 10;
        }
    }
    *milliseconds = result > 600000 ? 600000 : (uint32_t)result;
    return true;
}

static size_t checked_length(int written, size_t size) {
    return written < 0 || (size_t)written >= size ? 0 : (size_t)written;
}

size_t ha_light_state_json(const light_state_t *state, const char *effect, char *out, size_t size) {
    int written = snprintf(out, size,
                           "{\"state\":\"%s\",\"brightness\":%u,\"color_mode\":\"rgb\",\"color\":{\"r\":%u,\"g\":%u,"
                           "\"b\":%u}%s%s%s}",
                           state->on ? "ON" : "OFF", state->brightness, state->red, state->green, state->blue,
                           effect ? ",\"effect\":\"" : "", effect ? effect : "", effect ? "\"" : "");
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
    static char effects[512];  // static: keeps the discovery call chain small on the 4 KB main stack
    effects[0] = '\0';
    if (light->effect_count > 0) {
        size_t used = (size_t)snprintf(effects, sizeof effects, ",\"effect\":true,\"effect_list\":[");
        for (size_t i = 0; i < light->effect_count && used < sizeof effects; i++) {
            used += (size_t)snprintf(effects + used, sizeof effects - used, "%s\"%s\"", i ? "," : "",
                                     light->effect_names[i]);
        }
        if (used + 2 > sizeof effects) return 0;
        snprintf(effects + used, sizeof effects - used, "]");
    }
    int written = snprintf(out, size,
                           "{\"name\":\"%s\",\"unique_id\":\"%s\",\"schema\":\"json\",\"command_topic\":\"%s\","
                           "\"state_topic\":\"%s\",\"availability_topic\":\"%s\",\"brightness\":true,"
                           "\"supported_color_modes\":[\"rgb\"]%s,\"device\":{\"identifiers\":[\"%s\"],"
                           "\"name\":\"Nanoleaf Canvas\",\"manufacturer\":\"FckAhLeaf\","
                           "\"model\":\"Pico W panel controller\",\"sw_version\":\"%s\"}}",
                           light->name, light->unique_id, light->command_topic, light->state_topic,
                           light->availability_topic, effects, light->device_id, light->firmware_version);
    return checked_length(written, size);
}

size_t ha_rotation_discovery_json(const char *unique_id, const char *command_topic, const char *state_topic,
                                  const char *availability_topic, const char *device_id, char *out, size_t size) {
    int written = snprintf(out, size,
                           "{\"name\":\"Layout rotation\",\"unique_id\":\"%s\",\"command_topic\":\"%s\","
                           "\"state_topic\":\"%s\",\"availability_topic\":\"%s\",\"entity_category\":\"config\","
                           "\"icon\":\"mdi:rotate-right\",\"options\":[\"0\",\"90\",\"180\",\"270\"],"
                           "\"device\":{\"identifiers\":[\"%s\"]}}",
                           unique_id, command_topic, state_topic, availability_topic, device_id);
    return checked_length(written, size);
}
