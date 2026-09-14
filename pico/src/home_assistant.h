#pragma once

// Publishes the wall and each square as Home Assistant MQTT lights and applies
// their commands to the controller. Squares are identified by hardware ID, so
// re-arranging them keeps Home Assistant entities and state.
//
// Topics, with <device> = canvas_<board id>:
//   homeassistant/light/<device>/<wall|square id>/config   discovery (retained)
//   canvas/<board id>/<wall|square id>/set                  commands from HA
//   canvas/<board id>/<wall|square id>/state                state (retained)
//   canvas/<board id>/status                                 online / offline (last will)

#include <stddef.h>
#include <stdint.h>

#include "controller.h"

void home_assistant_init(controller_t *controller, const char *board_id, const char *firmware_version);

const char *home_assistant_client_id(void);
const char *home_assistant_subscription(void);
const char *home_assistant_availability_topic(void);

// net_message_fn
void home_assistant_on_message(const char *topic, const char *payload, size_t length);

void home_assistant_on_controller_event(controller_event_t event);

// Publishes pending discovery and state messages, one at a time; call every main-loop pass.
void home_assistant_poll(uint64_t now_us);
