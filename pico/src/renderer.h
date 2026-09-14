#pragma once

// Decides what every square shows at each frame: a fade between solid colours, or a running
// effect. Pure C so the host tests can run it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "effects.h"
#include "ha_light.h"
#include "layout.h"

typedef struct {
    size_t count;
    effect_point_t points[EFFECT_MAX_SQUARES];
    uint8_t shown[EFFECT_MAX_SQUARES][4];
    uint8_t fade_from[EFFECT_MAX_SQUARES][4];
    uint8_t fade_to[EFFECT_MAX_SQUARES][4];
    uint64_t fade_start_us;
    uint64_t fade_duration_us;
    effect_t effect;  // EFFECT_SOLID while fading or holding solid colours
    light_state_t effect_base;
    effect_memory_t memory;
    bool has_content;  // false until the first fade or effect, so squares keep their own look
} renderer_t;

void renderer_init(renderer_t *renderer, uint32_t seed);

// New square positions after a layout read or a rotation change. Colours stay with their bus index.
void renderer_set_layout(renderer_t *renderer, const layout_position_t *positions, size_t count,
                         unsigned quarter_turns);

// Stops any effect and fades each square from what it shows now to `targets` over `duration_us`
// (0 switches immediately).
void renderer_fade_to(renderer_t *renderer, const uint8_t targets[][4], size_t count, uint64_t now_us,
                      uint64_t duration_us);

// Runs an animated effect using `base` for on/off, brightness and colour. Calling it again with the
// same effect only updates `base`, so animations continue smoothly. EFFECT_SOLID is ignored.
void renderer_start_effect(renderer_t *renderer, effect_t effect, const light_state_t *base);

// Colours for every square at `now_us`. Returns false while nothing has been set yet.
bool renderer_frame(renderer_t *renderer, uint64_t now_us, uint8_t out[][4]);
