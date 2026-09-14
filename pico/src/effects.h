#pragma once

// Animated wall effects, rendered on the Pico at the controller's frame rate.
// Pure C (plus libm) so the host tests can run it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ha_light.h"
#include "layout.h"

#define EFFECT_MAX_SQUARES 64

typedef enum {
    EFFECT_SOLID,  // no animation: every square shows its own light state
    EFFECT_COLOUR_CYCLE,
    EFFECT_BREATHE,
    EFFECT_TWINKLE,
    EFFECT_RAINBOW_WAVE,
    EFFECT_COLOUR_WAVE,
    EFFECT_RIPPLE,
    EFFECT_FIRE,
    EFFECT_COUNT,
} effect_t;

// Square centres across the wall, 0..1 on each axis, x to the right and y upwards as the wall hangs.
typedef struct {
    float x;
    float y;
} effect_point_t;

// Per-effect state that carries over between frames.
typedef struct {
    uint32_t random;
    uint64_t last_frame_us;
    float sparkle[EFFECT_MAX_SQUARES];
    float heat[EFFECT_MAX_SQUARES];
} effect_memory_t;

const char *effect_name(effect_t effect);

// EFFECT_COUNT if no effect has this name.
effect_t effect_from_name(const char *name);

// Rotates layout positions clockwise by `quarter_turns` (the wall's orientation relative to the
// first square) and scales them to 0..1. An axis with a single row or column maps to 0.5.
void effect_points_from_layout(const layout_position_t *positions, size_t count, unsigned quarter_turns,
                               effect_point_t *points);

void effect_memory_init(effect_memory_t *memory, uint32_t seed);

// Colours for `count` squares at `now_us`. `base` supplies on/off, brightness and, for
// Breathe, Twinkle, Colour wave and Ripple, the colour. EFFECT_SOLID renders `base` everywhere.
void effect_render(effect_t effect, uint64_t now_us, const effect_point_t *points, size_t count,
                   const light_state_t *base, effect_memory_t *memory, uint8_t rgbw[][4]);
