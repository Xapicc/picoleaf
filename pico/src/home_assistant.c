#include "home_assistant.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config_flash.h"
#include "effects.h"
#include "ha_light.h"
#include "layout.h"
#include "net.h"
#include "pico/time.h"
#include "renderer.h"

#define UID_HEX_LENGTH (2 * CONTROLLER_UID_LENGTH)
#define TOPIC_MAX 128
#define EFFECT_NAME_MAX 32
// Spacing between publishes, so a burst after (re)connecting can't exhaust lwIP's buffers.
#define PUBLISH_INTERVAL_US 25000u

typedef struct {
    bool used;
    uint8_t uid[CONTROLLER_UID_LENGTH];
    char uid_hex[UID_HEX_LENGTH + 1];
    light_state_t state;
    bool discovery_pending;
    bool state_pending;
} square_t;

static struct {
    controller_t *controller;
    device_config_t *config;
    const char *firmware_version;
    char client_id[48];     // canvas_<board id>
    char base_topic[48];    // canvas/<board id>
    char subscription[64];  // canvas/<board id>/+/set
    char availability_topic[64];
    // Every square seen since boot, keyed by hardware ID.
    square_t known[CONTROLLER_MAX_SQUARES];
    // Bus index -> known[] slot for the current layout.
    int by_index[CONTROLLER_MAX_SQUARES];
    size_t index_count;
    layout_position_t positions[CONTROLLER_MAX_SQUARES];
    light_state_t wall;
    bool wall_commanded;
    effect_t effect;
    bool wall_discovery_pending;
    bool wall_state_pending;
    bool rotation_discovery_pending;
    bool rotation_state_pending;
    renderer_t renderer;
    uint64_t next_render_us;
    uint32_t published_for_connection;
    uint64_t next_publish_us;
} ha;

void home_assistant_init(controller_t *controller, device_config_t *config, const char *board_id,
                         const char *firmware_version, uint64_t now_us) {
    memset(&ha, 0, sizeof ha);
    ha.controller = controller;
    ha.config = config;
    ha.firmware_version = firmware_version;
    ha.wall = LIGHT_DEFAULT_STATE;
    ha.effect = EFFECT_SOLID;
    renderer_init(&ha.renderer, (uint32_t)now_us);
    snprintf(ha.client_id, sizeof ha.client_id, "canvas_%s", board_id);
    snprintf(ha.base_topic, sizeof ha.base_topic, "canvas/%s", board_id);
    snprintf(ha.subscription, sizeof ha.subscription, "%s/+/set", ha.base_topic);
    snprintf(ha.availability_topic, sizeof ha.availability_topic, "%s/status", ha.base_topic);
}

const char *home_assistant_client_id(void) {
    return ha.client_id;
}

const char *home_assistant_subscription(void) {
    return ha.subscription;
}

const char *home_assistant_availability_topic(void) {
    return ha.availability_topic;
}

static square_t *find_or_add(const uint8_t *uid) {
    square_t *free_slot = NULL;
    for (size_t i = 0; i < CONTROLLER_MAX_SQUARES; i++) {
        if (ha.known[i].used && memcmp(ha.known[i].uid, uid, CONTROLLER_UID_LENGTH) == 0) return &ha.known[i];
        if (!ha.known[i].used && free_slot == NULL) free_slot = &ha.known[i];
    }
    if (free_slot == NULL) return NULL;
    free_slot->used = true;
    memcpy(free_slot->uid, uid, CONTROLLER_UID_LENGTH);
    for (size_t i = 0; i < CONTROLLER_UID_LENGTH; i++) sprintf(free_slot->uid_hex + 2 * i, "%02x", uid[i]);
    free_slot->state = ha.wall_commanded ? ha.wall : LIGHT_DEFAULT_STATE;
    return free_slot;
}

// Fades every square to its own light state (the solid, no-effect picture).
static void fade_to_square_states(uint64_t now_us, uint32_t transition_ms) {
    uint8_t targets[CONTROLLER_MAX_SQUARES][4] = {{0}};
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] >= 0) ha_light_output(&ha.known[ha.by_index[index]].state, targets[index]);
    }
    renderer_fade_to(&ha.renderer, targets, ha.index_count, now_us, (uint64_t)transition_ms * 1000u);
}

static void apply_layout_rotation(void) {
    renderer_set_layout(&ha.renderer, ha.positions, ha.index_count, ha.config->layout_rotation / 90u);
}

static void mark_everything_pending(void) {
    ha.wall_discovery_pending = ha.wall_state_pending = true;
    ha.rotation_discovery_pending = ha.rotation_state_pending = true;
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] < 0) continue;
        ha.known[ha.by_index[index]].discovery_pending = true;
        ha.known[ha.by_index[index]].state_pending = true;
    }
}

void home_assistant_on_controller_event(controller_event_t event, uint64_t now_us) {
    if (event != CONTROLLER_UIDS_READY) return;
    ha.index_count = ha.controller->square_count;
    for (size_t index = 0; index < ha.index_count; index++) {
        square_t *square = find_or_add(controller_uid(ha.controller, index));
        ha.by_index[index] = square ? (int)(square - ha.known) : -1;
    }
    int placed = layout_positions(ha.controller->layout, ha.controller->layout_length, ha.positions,
                                  CONTROLLER_MAX_SQUARES);
    if (placed != (int)ha.index_count) {
        // Effects still run, but position-based ones treat every square as the same spot.
        printf("HA layout reply didn't parse into %u squares; positional effects will look flat\n",
               (unsigned)ha.index_count);
        memset(ha.positions, 0, sizeof ha.positions);
    }
    apply_layout_rotation();
    mark_everything_pending();
    // Re-map colours to the new bus order, but only if Home Assistant has taken over the squares.
    if (ha.renderer.has_content && ha.effect == EFFECT_SOLID) fade_to_square_states(now_us, 0);
}

static void handle_rotation_command(const char *payload) {
    if (!config_set_field(ha.config, "layout_rotation", payload)) {
        printf("HA ignored layout rotation '%s' (expected 0, 90, 180 or 270)\n", payload);
        return;
    }
    if (!config_flash_save(ha.config)) printf("HA layout rotation applied but could not be saved to flash\n");
    apply_layout_rotation();
    ha.rotation_state_pending = true;
}

static void handle_wall_command(const char *payload, size_t length, uint64_t now_us) {
    if (!ha_light_apply_command(&ha.wall, payload, length)) {
        printf("HA ignored wall command: %s\n", payload);
        return;
    }
    ha.wall_commanded = true;
    ha.wall_state_pending = true;

    char effect_text[EFFECT_NAME_MAX];
    if (ha_light_command_effect(payload, length, effect_text, sizeof effect_text)) {
        effect_t requested = effect_from_name(effect_text);
        if (requested == EFFECT_COUNT) {
            printf("HA ignored unknown effect '%s'\n", effect_text);
        } else {
            ha.effect = requested;
        }
    }
    uint32_t transition_ms = 0;
    ha_light_command_transition_ms(payload, length, &transition_ms);

    if (ha.effect != EFFECT_SOLID) {
        renderer_start_effect(&ha.renderer, ha.effect, &ha.wall);
        return;
    }
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] < 0) continue;
        square_t *square = &ha.known[ha.by_index[index]];
        ha_light_apply_command(&square->state, payload, length);
        square->state_pending = true;
    }
    fade_to_square_states(now_us, transition_ms);
}

static void handle_square_command(const char *target, size_t target_length, const char *payload, size_t length,
                                  uint64_t now_us) {
    square_t *square = NULL;
    for (size_t i = 0; i < CONTROLLER_MAX_SQUARES; i++) {
        if (ha.known[i].used && target_length == UID_HEX_LENGTH &&
            strncmp(ha.known[i].uid_hex, target, UID_HEX_LENGTH) == 0) {
            square = &ha.known[i];
        }
    }
    if (square == NULL || !ha_light_apply_command(&square->state, payload, length)) {
        printf("HA ignored square command: %s\n", payload);
        return;
    }
    square->state_pending = true;
    if (ha.effect != EFFECT_SOLID) {
        // Controlling one square ends the wall effect so the change is visible.
        ha.effect = EFFECT_SOLID;
        ha.wall_state_pending = true;
    }
    uint32_t transition_ms = 0;
    ha_light_command_transition_ms(payload, length, &transition_ms);
    fade_to_square_states(now_us, transition_ms);
}

void home_assistant_on_message(const char *topic, const char *payload, size_t length) {
    size_t base_length = strlen(ha.base_topic);
    if (strncmp(topic, ha.base_topic, base_length) != 0 || topic[base_length] != '/') return;
    const char *target = topic + base_length + 1;
    const char *suffix = strchr(target, '/');
    if (suffix == NULL || strcmp(suffix, "/set") != 0) return;
    size_t target_length = (size_t)(suffix - target);
    uint64_t now_us = time_us_64();

    if (target_length == 4 && strncmp(target, "wall", 4) == 0) {
        handle_wall_command(payload, length, now_us);
    } else if (target_length == 15 && strncmp(target, "layout_rotation", 15) == 0) {
        handle_rotation_command(payload);
    } else {
        handle_square_command(target, target_length, payload, length, now_us);
    }
}

void home_assistant_render(uint64_t now_us) {
    if (now_us < ha.next_render_us) return;
    ha.next_render_us = now_us + CONTROLLER_TICK_US;
    uint8_t frame[CONTROLLER_MAX_SQUARES][4];
    if (!renderer_frame(&ha.renderer, now_us, frame)) return;
    for (size_t index = 0; index < ha.index_count; index++) controller_set(ha.controller, index, frame[index]);
}

static bool publish_light_discovery(const char *object_id, const char *name, bool with_effects) {
    char topic[TOPIC_MAX], command_topic[TOPIC_MAX], state_topic[TOPIC_MAX], unique_id[TOPIC_MAX];
    snprintf(topic, sizeof topic, "homeassistant/light/%s/%s/config", ha.client_id, object_id);
    snprintf(command_topic, sizeof command_topic, "%s/%s/set", ha.base_topic, object_id);
    snprintf(state_topic, sizeof state_topic, "%s/%s/state", ha.base_topic, object_id);
    // "_light_" was added once to drop entity IDs generated from earlier, longer names; keep it stable now.
    snprintf(unique_id, sizeof unique_id, "%s_light_%s", ha.client_id, object_id);
    const char *effect_names[EFFECT_COUNT];
    for (int effect = 0; effect < EFFECT_COUNT; effect++) effect_names[effect] = effect_name((effect_t)effect);
    const ha_light_discovery_t light = {
        .name = name,
        .unique_id = unique_id,
        .command_topic = command_topic,
        .state_topic = state_topic,
        .availability_topic = ha.availability_topic,
        .device_id = ha.client_id,
        .firmware_version = ha.firmware_version,
        .effect_names = with_effects ? effect_names : NULL,
        .effect_count = with_effects ? EFFECT_COUNT : 0,
    };
    static char payload[1024];  // static: too big for the 4 KB main stack; only the main loop publishes
    if (ha_light_discovery_json(&light, payload, sizeof payload) == 0) return false;
    return net_publish(topic, payload, true);
}

static bool publish_light_state(const char *object_id, const light_state_t *state, const char *effect) {
    char topic[TOPIC_MAX], payload[160];
    snprintf(topic, sizeof topic, "%s/%s/state", ha.base_topic, object_id);
    if (ha_light_state_json(state, effect, payload, sizeof payload) == 0) return false;
    return net_publish(topic, payload, true);
}

static bool publish_rotation_discovery(void) {
    char topic[TOPIC_MAX], command_topic[TOPIC_MAX], state_topic[TOPIC_MAX], unique_id[TOPIC_MAX];
    static char payload[512];
    snprintf(topic, sizeof topic, "homeassistant/select/%s/layout_rotation/config", ha.client_id);
    snprintf(command_topic, sizeof command_topic, "%s/layout_rotation/set", ha.base_topic);
    snprintf(state_topic, sizeof state_topic, "%s/layout_rotation/state", ha.base_topic);
    snprintf(unique_id, sizeof unique_id, "%s_select_layout_rotation", ha.client_id);
    if (ha_rotation_discovery_json(unique_id, command_topic, state_topic, ha.availability_topic, ha.client_id,
                                   payload, sizeof payload) == 0) {
        return false;
    }
    return net_publish(topic, payload, true);
}

static bool publish_rotation_state(void) {
    char topic[TOPIC_MAX], payload[8];
    snprintf(topic, sizeof topic, "%s/layout_rotation/state", ha.base_topic);
    snprintf(payload, sizeof payload, "%u", ha.config->layout_rotation);
    return net_publish(topic, payload, true);
}

// Publishes the next pending message, if any. Returns false when nothing was attempted.
static bool publish_next(void) {
    if (ha.wall_discovery_pending) {
        // Home Assistant prefixes the device name ("Nanoleaf Canvas"), so keep entity names short.
        ha.wall_discovery_pending = !publish_light_discovery("wall", "Wall", true);
        return true;
    }
    if (ha.wall_state_pending) {
        ha.wall_state_pending = !publish_light_state("wall", &ha.wall, effect_name(ha.effect));
        return true;
    }
    if (ha.rotation_discovery_pending) {
        ha.rotation_discovery_pending = !publish_rotation_discovery();
        return true;
    }
    if (ha.rotation_state_pending) {
        ha.rotation_state_pending = !publish_rotation_state();
        return true;
    }
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] < 0) continue;
        square_t *square = &ha.known[ha.by_index[index]];
        if (square->discovery_pending) {
            char name[32];
            snprintf(name, sizeof name, "Tile %u", (unsigned)index);
            square->discovery_pending = !publish_light_discovery(square->uid_hex, name, false);
            return true;
        }
        if (square->state_pending) {
            square->state_pending = !publish_light_state(square->uid_hex, &square->state, NULL);
            return true;
        }
    }
    return false;
}

void home_assistant_poll(uint64_t now_us) {
    if (!net_mqtt_connected()) return;
    if (ha.published_for_connection != net_connection_count()) {
        ha.published_for_connection = net_connection_count();
        mark_everything_pending();
    }
    // A failed publish stays pending and is retried at the next interval.
    if (now_us < ha.next_publish_us) return;
    if (publish_next()) ha.next_publish_us = now_us + PUBLISH_INTERVAL_US;
}
