#include "renderer.h"

#include <string.h>

#define BOOT_SETTLE_US 1000000u
#define BLINK_US 200000u  // each flash, and each gap between flashes

void renderer_init(renderer_t *renderer, uint32_t seed) {
    memset(renderer, 0, sizeof *renderer);
    renderer->effect = EFFECT_SOLID;
    effect_memory_init(&renderer->memory, seed);
}

void renderer_set_layout(renderer_t *renderer, const layout_position_t *positions, size_t count,
                         unsigned quarter_turns) {
    if (count > EFFECT_MAX_SQUARES) count = EFFECT_MAX_SQUARES;
    renderer->count = count;
    effect_points_from_layout(positions, count, quarter_turns, renderer->points);
}

static void stop_startup(renderer_t *renderer) {
    renderer->startup_active = renderer->shimmering = renderer->blink_queued = renderer->blinking = false;
}

void renderer_fade_to(renderer_t *renderer, const uint8_t targets[][4], size_t count, uint64_t now_us,
                      uint64_t duration_us) {
    if (count > EFFECT_MAX_SQUARES) count = EFFECT_MAX_SQUARES;
    memcpy(renderer->fade_from, renderer->shown, sizeof renderer->fade_from);
    memcpy(renderer->fade_to, targets, count * sizeof targets[0]);
    renderer->fade_start_us = now_us;
    renderer->fade_duration_us = duration_us;
    renderer->effect = EFFECT_SOLID;
    renderer->has_content = true;
    stop_startup(renderer);
}

void renderer_start_effect(renderer_t *renderer, effect_t effect, const light_state_t *base) {
    if (effect == EFFECT_SOLID || effect >= EFFECT_COUNT) return;
    if (effect != renderer->effect) effect_memory_init(&renderer->memory, renderer->memory.random);
    renderer->effect = effect;
    renderer->effect_base = *base;
    renderer->has_content = true;
    stop_startup(renderer);
}

static void extend_startup(renderer_t *renderer, uint64_t end_us) {
    renderer->startup_active = true;
    if (end_us > renderer->startup_end_us) renderer->startup_end_us = end_us;
}

void renderer_start_boot_shimmer(renderer_t *renderer, const uint8_t rest[][4], size_t count, uint64_t now_us,
                                 uint64_t duration_us) {
    if (renderer->has_content) return;
    if (count > EFFECT_MAX_SQUARES) count = EFFECT_MAX_SQUARES;
    memcpy(renderer->fade_to, rest, count * sizeof rest[0]);
    renderer->has_rest = true;
    renderer->shimmering = true;
    renderer->shimmer_start_us = now_us;
    renderer->shimmer_duration_us = duration_us;
    extend_startup(renderer, now_us + duration_us);
}

static uint64_t blink_length_us(const renderer_t *renderer) {
    return (2u * (uint64_t)renderer->blink_times - 1u) * BLINK_US;
}

static void start_blink(renderer_t *renderer, uint64_t now_us) {
    renderer->blinking = true;
    renderer->blink_start_us = now_us;
    extend_startup(renderer, now_us + blink_length_us(renderer));
}

void renderer_blink(renderer_t *renderer, uint64_t now_us, const uint8_t colour[4], unsigned times) {
    if ((!renderer->has_content && !renderer->has_rest) || times == 0) return;
    memcpy(renderer->blink_colour, colour, sizeof renderer->blink_colour);
    renderer->blink_times = times;
    if (renderer->shimmering) {
        renderer->blink_queued = true;
    } else {
        start_blink(renderer, now_us);
    }
}

static uint64_t elapsed_since(uint64_t now_us, uint64_t start_us) {
    return now_us > start_us ? now_us - start_us : 0;
}

static void end_shimmer(renderer_t *renderer, uint64_t now_us) {
    renderer->shimmering = false;
    memcpy(renderer->fade_from, renderer->shown, sizeof renderer->fade_from);
    renderer->fade_start_us = now_us;
    renderer->fade_duration_us = BOOT_SETTLE_US;
    extend_startup(renderer, now_us + BOOT_SETTLE_US);
    if (renderer->blink_queued) {
        renderer->blink_queued = false;
        start_blink(renderer, now_us);
    }
}

static uint8_t mix(uint8_t from, uint8_t to, uint64_t done, uint64_t total) {
    int64_t scaled = ((int64_t)to - (int64_t)from) * (int64_t)done;
    // Round half away from zero; C division truncates towards zero, which would bias downward fades.
    int64_t half = (int64_t)total / 2;
    int64_t step = (scaled >= 0 ? scaled + half : scaled - half) / (int64_t)total;
    return (uint8_t)((int64_t)from + step);
}

bool renderer_frame(renderer_t *renderer, uint64_t now_us, uint8_t out[][4]) {
    if (!renderer->has_content && !renderer->startup_active) return false;
    if (renderer->shimmering && elapsed_since(now_us, renderer->shimmer_start_us) >= renderer->shimmer_duration_us) {
        end_shimmer(renderer, now_us);
    }
    if (renderer->shimmering) {
        effect_render_boot_shimmer(elapsed_since(now_us, renderer->shimmer_start_us), renderer->points,
                                   renderer->count, out);
    } else if (renderer->effect != EFFECT_SOLID) {
        effect_render(renderer->effect, now_us, renderer->points, renderer->count, &renderer->effect_base,
                      &renderer->memory, out);
    } else {
        uint64_t elapsed = elapsed_since(now_us, renderer->fade_start_us);
        bool finished = renderer->fade_duration_us == 0 || elapsed >= renderer->fade_duration_us;
        for (size_t i = 0; i < renderer->count; i++) {
            for (int channel = 0; channel < 4; channel++) {
                out[i][channel] = finished ? renderer->fade_to[i][channel]
                                           : mix(renderer->fade_from[i][channel], renderer->fade_to[i][channel],
                                                 elapsed, renderer->fade_duration_us);
            }
        }
    }
    uint64_t blink_elapsed = elapsed_since(now_us, renderer->blink_start_us);
    if (renderer->blinking && blink_elapsed >= blink_length_us(renderer)) renderer->blinking = false;
    if (renderer->blinking && (blink_elapsed / BLINK_US) % 2 == 0) {
        for (size_t i = 0; i < renderer->count; i++) memcpy(out[i], renderer->blink_colour, 4);
    }
    // The frame at or after the end is still sent, so the squares are left on the settled colours.
    if (renderer->startup_active && !renderer->shimmering && now_us >= renderer->startup_end_us) {
        renderer->startup_active = false;
    }
    memcpy(renderer->shown, out, renderer->count * sizeof out[0]);
    return true;
}
