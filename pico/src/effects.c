#include "effects.h"

#include <math.h>
#include <string.h>

#define TWO_PI 6.2831853f

// Periods in seconds, chosen to look calm on a wall-sized display.
#define COLOUR_CYCLE_PERIOD 20.0f
#define BREATHE_PERIOD 4.0f
#define RAINBOW_WAVE_PERIOD 6.0f
#define COLOUR_WAVE_PERIOD 3.0f
#define RIPPLE_PERIOD 2.5f
#define TWINKLE_RATE_PER_SECOND 0.4f  // chance per square per second of starting a sparkle
#define TWINKLE_DECAY_SECONDS 0.5f
#define TWINKLE_BACKGROUND 0.3f
#define FIRE_RESPONSE_SECONDS 0.15f

static const char *NAMES[EFFECT_COUNT] = {
    "Solid", "Colour cycle", "Breathe", "Twinkle", "Rainbow wave", "Colour wave", "Ripple", "Fire",
};

const char *effect_name(effect_t effect) {
    return effect < EFFECT_COUNT ? NAMES[effect] : "";
}

effect_t effect_from_name(const char *name) {
    for (int effect = 0; effect < EFFECT_COUNT; effect++) {
        if (strcmp(NAMES[effect], name) == 0) return (effect_t)effect;
    }
    return EFFECT_COUNT;
}

void effect_points_from_layout(const layout_position_t *positions, size_t count, unsigned quarter_turns,
                               effect_point_t *points) {
    if (count == 0) return;
    int turned_x[EFFECT_MAX_SQUARES], turned_y[EFFECT_MAX_SQUARES];
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (size_t i = 0; i < count && i < EFFECT_MAX_SQUARES; i++) {
        int x = positions[i].x, y = positions[i].y;
        for (unsigned turn = 0; turn < quarter_turns % 4; turn++) {
            int previous_x = x;
            x = y;
            y = -previous_x;
        }
        turned_x[i] = x;
        turned_y[i] = y;
        if (i == 0 || x < min_x) min_x = x;
        if (i == 0 || x > max_x) max_x = x;
        if (i == 0 || y < min_y) min_y = y;
        if (i == 0 || y > max_y) max_y = y;
    }
    for (size_t i = 0; i < count && i < EFFECT_MAX_SQUARES; i++) {
        points[i].x = max_x == min_x ? 0.5f : (float)(turned_x[i] - min_x) / (float)(max_x - min_x);
        points[i].y = max_y == min_y ? 0.5f : (float)(turned_y[i] - min_y) / (float)(max_y - min_y);
    }
}

void effect_memory_init(effect_memory_t *memory, uint32_t seed) {
    memset(memory, 0, sizeof *memory);
    memory->random = seed ? seed : 0x9E3779B9u;
}

// xorshift32: deterministic for tests, plenty for flicker.
static float next_random(effect_memory_t *memory) {
    uint32_t value = memory->random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    memory->random = value;
    return (float)(value >> 8) / 16777216.0f;
}

static float wrap(float value) {
    return value - floorf(value);
}

static float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static uint8_t to_byte(float value) {
    return (uint8_t)lroundf(clamp01(value) * 255.0f);
}

// Hue 0..1, full saturation, value 0..1.
static void hue_to_rgb(float hue, float value, float rgb[3]) {
    float sector = wrap(hue) * 6.0f;
    int index = (int)sector;
    float fraction = sector - (float)index;
    float rising = value * fraction, falling = value * (1.0f - fraction);
    switch (index % 6) {
        case 0: rgb[0] = value, rgb[1] = rising, rgb[2] = 0; break;
        case 1: rgb[0] = falling, rgb[1] = value, rgb[2] = 0; break;
        case 2: rgb[0] = 0, rgb[1] = value, rgb[2] = rising; break;
        case 3: rgb[0] = 0, rgb[1] = falling, rgb[2] = value; break;
        case 4: rgb[0] = rising, rgb[1] = 0, rgb[2] = value; break;
        default: rgb[0] = value, rgb[1] = 0, rgb[2] = falling; break;
    }
}

// Black → red → orange → yellow.
static void heat_to_rgb(float heat, float rgb[3]) {
    heat = clamp01(heat);
    rgb[0] = clamp01(heat * 2.0f);
    rgb[1] = clamp01((heat - 0.4f) * 1.6f);
    rgb[2] = clamp01((heat - 0.85f) * 3.0f) * 0.4f;
}

static void write(uint8_t rgbw[4], const float rgb[3], float brightness) {
    rgbw[0] = to_byte(rgb[0] * brightness);
    rgbw[1] = to_byte(rgb[1] * brightness);
    rgbw[2] = to_byte(rgb[2] * brightness);
    rgbw[3] = 0;
}

void effect_render(effect_t effect, uint64_t now_us, const effect_point_t *points, size_t count,
                   const light_state_t *base, effect_memory_t *memory, uint8_t rgbw[][4]) {
    if (count > EFFECT_MAX_SQUARES) count = EFFECT_MAX_SQUARES;
    float seconds = (float)(now_us % 3600000000ull) / 1e6f;  // wraps hourly; all periods divide an hour
    float elapsed = memory->last_frame_us == 0 || now_us < memory->last_frame_us
                        ? 0.0f
                        : (float)(now_us - memory->last_frame_us) / 1e6f;
    memory->last_frame_us = now_us;

    float brightness = base->on ? (float)base->brightness / 255.0f : 0.0f;
    const float colour[3] = {base->red / 255.0f, base->green / 255.0f, base->blue / 255.0f};

    for (size_t i = 0; i < count; i++) {
        float rgb[3], level;
        const effect_point_t *point = &points[i];
        switch (effect) {
            case EFFECT_COLOUR_CYCLE:
                hue_to_rgb(seconds / COLOUR_CYCLE_PERIOD, 1.0f, rgb);
                break;
            case EFFECT_BREATHE:
                level = 0.15f + 0.85f * (0.5f - 0.5f * cosf(TWO_PI * seconds / BREATHE_PERIOD));
                for (int c = 0; c < 3; c++) rgb[c] = colour[c] * level;
                break;
            case EFFECT_TWINKLE:
                memory->sparkle[i] *= expf(-elapsed / TWINKLE_DECAY_SECONDS);
                if (next_random(memory) < TWINKLE_RATE_PER_SECOND * elapsed) memory->sparkle[i] = 1.0f;
                for (int c = 0; c < 3; c++) {
                    float background = colour[c] * TWINKLE_BACKGROUND;
                    rgb[c] = background + (1.0f - background) * memory->sparkle[i];
                }
                break;
            case EFFECT_RAINBOW_WAVE:
                hue_to_rgb(point->x * 0.8f - seconds / RAINBOW_WAVE_PERIOD, 1.0f, rgb);
                break;
            case EFFECT_COLOUR_WAVE:
                // Crests travel upwards.
                level = 0.2f + 0.8f * (0.5f + 0.5f * sinf(TWO_PI * (point->y - seconds / COLOUR_WAVE_PERIOD)));
                for (int c = 0; c < 3; c++) rgb[c] = colour[c] * level;
                break;
            case EFFECT_RIPPLE: {
                float dx = point->x - 0.5f, dy = point->y - 0.5f;
                float distance = sqrtf(dx * dx + dy * dy);
                // Rings travel outwards from the middle of the wall.
                level = 0.15f + 0.85f * (0.5f + 0.5f * cosf(TWO_PI * (distance * 2.0f - seconds / RIPPLE_PERIOD)));
                for (int c = 0; c < 3; c++) rgb[c] = colour[c] * level;
                break;
            }
            case EFFECT_FIRE: {
                // Hottest along the bottom, flickering towards a random target each frame.
                float target = (1.0f - 0.75f * point->y) * (0.55f + 0.45f * next_random(memory));
                float response = elapsed <= 0.0f ? 1.0f : clamp01(elapsed / FIRE_RESPONSE_SECONDS);
                memory->heat[i] += (target - memory->heat[i]) * response;
                heat_to_rgb(memory->heat[i], rgb);
                break;
            }
            default:
                memcpy(rgb, colour, sizeof rgb);
                break;
        }
        write(rgbw[i], rgb, brightness);
    }
}
