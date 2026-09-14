#include "renderer.h"

#include <string.h>

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

void renderer_fade_to(renderer_t *renderer, const uint8_t targets[][4], size_t count, uint64_t now_us,
                      uint64_t duration_us) {
    if (count > EFFECT_MAX_SQUARES) count = EFFECT_MAX_SQUARES;
    memcpy(renderer->fade_from, renderer->shown, sizeof renderer->fade_from);
    memcpy(renderer->fade_to, targets, count * sizeof targets[0]);
    renderer->fade_start_us = now_us;
    renderer->fade_duration_us = duration_us;
    renderer->effect = EFFECT_SOLID;
    renderer->has_content = true;
}

void renderer_start_effect(renderer_t *renderer, effect_t effect, const light_state_t *base) {
    if (effect == EFFECT_SOLID || effect >= EFFECT_COUNT) return;
    if (effect != renderer->effect) effect_memory_init(&renderer->memory, renderer->memory.random);
    renderer->effect = effect;
    renderer->effect_base = *base;
    renderer->has_content = true;
}

static uint8_t mix(uint8_t from, uint8_t to, uint64_t done, uint64_t total) {
    int64_t scaled = ((int64_t)to - (int64_t)from) * (int64_t)done;
    // Round half away from zero; C division truncates towards zero, which would bias downward fades.
    int64_t half = (int64_t)total / 2;
    int64_t step = (scaled >= 0 ? scaled + half : scaled - half) / (int64_t)total;
    return (uint8_t)((int64_t)from + step);
}

bool renderer_frame(renderer_t *renderer, uint64_t now_us, uint8_t out[][4]) {
    if (!renderer->has_content) return false;
    if (renderer->effect != EFFECT_SOLID) {
        effect_render(renderer->effect, now_us, renderer->points, renderer->count, &renderer->effect_base,
                      &renderer->memory, out);
    } else {
        uint64_t elapsed = now_us > renderer->fade_start_us ? now_us - renderer->fade_start_us : 0;
        bool finished = renderer->fade_duration_us == 0 || elapsed >= renderer->fade_duration_us;
        for (size_t i = 0; i < renderer->count; i++) {
            for (int channel = 0; channel < 4; channel++) {
                out[i][channel] = finished ? renderer->fade_to[i][channel]
                                           : mix(renderer->fade_from[i][channel], renderer->fade_to[i][channel],
                                                 elapsed, renderer->fade_duration_us);
            }
        }
    }
    memcpy(renderer->shown, out, renderer->count * sizeof out[0]);
    return true;
}
