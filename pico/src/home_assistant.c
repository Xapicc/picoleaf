#include "home_assistant.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ha_light.h"
#include "net.h"

#define UID_HEX_LENGTH (2 * CONTROLLER_UID_LENGTH)
#define TOPIC_MAX 128
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
    light_state_t wall;
    bool wall_commanded;
    bool wall_discovery_pending;
    bool wall_state_pending;
    // Nothing is pushed to the squares until Home Assistant sends a command.
    bool outputs_active;
    uint32_t published_for_connection;
    uint64_t next_publish_us;
} ha;

void home_assistant_init(controller_t *controller, const char *board_id, const char *firmware_version) {
    memset(&ha, 0, sizeof ha);
    ha.controller = controller;
    ha.firmware_version = firmware_version;
    ha.wall = LIGHT_DEFAULT_STATE;
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

static void push_outputs(void) {
    if (!ha.outputs_active) return;
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] < 0) continue;
        uint8_t rgbw[4];
        ha_light_output(&ha.known[ha.by_index[index]].state, rgbw);
        controller_set(ha.controller, index, rgbw);
    }
}

static void mark_everything_pending(void) {
    ha.wall_discovery_pending = ha.wall_state_pending = true;
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] < 0) continue;
        ha.known[ha.by_index[index]].discovery_pending = true;
        ha.known[ha.by_index[index]].state_pending = true;
    }
}

void home_assistant_on_controller_event(controller_event_t event) {
    if (event != CONTROLLER_UIDS_READY) return;
    ha.index_count = ha.controller->square_count;
    for (size_t index = 0; index < ha.index_count; index++) {
        square_t *square = find_or_add(controller_uid(ha.controller, index));
        ha.by_index[index] = square ? (int)(square - ha.known) : -1;
    }
    mark_everything_pending();
    push_outputs();
}

void home_assistant_on_message(const char *topic, const char *payload, size_t length) {
    size_t base_length = strlen(ha.base_topic);
    if (strncmp(topic, ha.base_topic, base_length) != 0 || topic[base_length] != '/') return;
    const char *target = topic + base_length + 1;
    const char *suffix = strchr(target, '/');
    if (suffix == NULL || strcmp(suffix, "/set") != 0) return;
    size_t target_length = (size_t)(suffix - target);

    if (target_length == 4 && strncmp(target, "wall", 4) == 0) {
        if (!ha_light_apply_command(&ha.wall, payload, length)) {
            printf("HA ignored wall command: %s\n", payload);
            return;
        }
        ha.wall_commanded = true;
        ha.wall_state_pending = true;
        for (size_t index = 0; index < ha.index_count; index++) {
            if (ha.by_index[index] < 0) continue;
            square_t *square = &ha.known[ha.by_index[index]];
            ha_light_apply_command(&square->state, payload, length);
            square->state_pending = true;
        }
    } else {
        square_t *square = NULL;
        for (size_t i = 0; i < CONTROLLER_MAX_SQUARES; i++) {
            if (ha.known[i].used && target_length == UID_HEX_LENGTH &&
                strncmp(ha.known[i].uid_hex, target, UID_HEX_LENGTH) == 0) {
                square = &ha.known[i];
            }
        }
        if (square == NULL || !ha_light_apply_command(&square->state, payload, length)) {
            printf("HA ignored command on %s: %s\n", topic, payload);
            return;
        }
        square->state_pending = true;
    }
    ha.outputs_active = true;
    push_outputs();
}

static bool publish_discovery(const char *object_id, const char *name) {
    char topic[TOPIC_MAX], command_topic[TOPIC_MAX], state_topic[TOPIC_MAX], unique_id[TOPIC_MAX];
    snprintf(topic, sizeof topic, "homeassistant/light/%s/%s/config", ha.client_id, object_id);
    snprintf(command_topic, sizeof command_topic, "%s/%s/set", ha.base_topic, object_id);
    snprintf(state_topic, sizeof state_topic, "%s/%s/state", ha.base_topic, object_id);
    // "_light_" was added once to drop entity IDs generated from earlier, longer names; keep it stable now.
    snprintf(unique_id, sizeof unique_id, "%s_light_%s", ha.client_id, object_id);
    const ha_light_discovery_t light = {
        .name = name,
        .unique_id = unique_id,
        .command_topic = command_topic,
        .state_topic = state_topic,
        .availability_topic = ha.availability_topic,
        .device_id = ha.client_id,
        .firmware_version = ha.firmware_version,
    };
    char payload[768];
    if (ha_light_discovery_json(&light, payload, sizeof payload) == 0) return false;
    return net_publish(topic, payload, true);
}

static bool publish_state(const char *object_id, const light_state_t *state) {
    char topic[TOPIC_MAX], payload[128];
    snprintf(topic, sizeof topic, "%s/%s/state", ha.base_topic, object_id);
    if (ha_light_state_json(state, payload, sizeof payload) == 0) return false;
    return net_publish(topic, payload, true);
}

// Publishes the next pending message, if any. Returns false when nothing was attempted.
static bool publish_next(void) {
    if (ha.wall_discovery_pending) {
        // Home Assistant prefixes the device name ("Nanoleaf Canvas"), so keep entity names short.
        ha.wall_discovery_pending = !publish_discovery("wall", "Wall");
        return true;
    }
    if (ha.wall_state_pending) {
        ha.wall_state_pending = !publish_state("wall", &ha.wall);
        return true;
    }
    for (size_t index = 0; index < ha.index_count; index++) {
        if (ha.by_index[index] < 0) continue;
        square_t *square = &ha.known[ha.by_index[index]];
        if (square->discovery_pending) {
            char name[32];
            snprintf(name, sizeof name, "Tile %u", (unsigned)index);
            square->discovery_pending = !publish_discovery(square->uid_hex, name);
            return true;
        }
        if (square->state_pending) {
            square->state_pending = !publish_state(square->uid_hex, &square->state);
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
