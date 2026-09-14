#pragma once

// Publishes the wall and each square as Home Assistant MQTT lights, plus a layout-rotation
// select, and turns their commands into fades and effects on the controller. Squares are
// identified by hardware ID, so re-arranging them keeps Home Assistant entities and state.
//
// Topics, with <device> = canvas_<board id>:
//   homeassistant/light/<device>/<wall|square id>/config      light discovery (retained)
//   homeassistant/select/<device>/layout_rotation/config      rotation select discovery (retained)
//   canvas/<board id>/<wall|square id|layout_rotation>/set     commands from HA
//   canvas/<board id>/<wall|square id|layout_rotation>/state   state (retained)
//   canvas/<board id>/status                                    online / offline (last will)

#include <stddef.h>
#include <stdint.h>

#include "config_record.h"
#include "controller.h"

// `config` supplies layout_rotation and is saved to flash when Home Assistant changes it.
void home_assistant_init(controller_t *controller, device_config_t *config, const char *board_id,
                         const char *firmware_version, uint64_t now_us);

const char *home_assistant_client_id(void);
const char *home_assistant_subscription(void);
const char *home_assistant_availability_topic(void);

// net_message_fn
void home_assistant_on_message(const char *topic, const char *payload, size_t length);

void home_assistant_on_controller_event(controller_event_t event, uint64_t now_us);

// Computes the next frame (fade or effect) and hands it to the controller, at the controller's rate.
void home_assistant_render(uint64_t now_us);

// Blinks the wall for MQTT connection changes and publishes pending discovery and state messages, one
// at a time. Call every main-loop pass, also when the network couldn't be started, so that shows as a problem.
void home_assistant_poll(uint64_t now_us);
