#include "uart_decode.h"

static uint32_t edge_tick(const uint32_t *words, size_t index) {
    return 0xFFFFFFFFu - words[index];
}

// Level at `tick`: the start level flipped once per edge at or before it.
static int level_at(const uint32_t *words, size_t count, uint64_t tick) {
    size_t low = 1;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (edge_tick(words, middle) <= tick) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    size_t edges = low - 1;
    return (int)((words[0] ^ edges) & 1u);
}

size_t uart_decode(const uint32_t *words, size_t count, uint32_t tick_hz, uint32_t baud, uint8_t *values,
                   bool *stop_ok, size_t max) {
    if (count < 2 || baud == 0) return 0;

    // Positions are kept in 1/16 tick: 1 Mbaud is 62.5 ticks per bit, so whole
    // ticks would drift by several ticks over a byte.
    uint64_t bit16 = (uint64_t)tick_hz * 16u / baud;
    uint64_t earliest16 = 0;
    size_t decoded = 0;

    for (size_t index = 1; index < count && decoded < max; index++) {
        uint64_t edge16 = (uint64_t)edge_tick(words, index) * 16u;
        if (edge16 < earliest16) continue;
        if (level_at(words, count, edge16 / 16u) == 1) continue;  // rising edge
        if (level_at(words, count, (edge16 + bit16 / 2u) / 16u) == 1) continue;  // glitch

        uint8_t value = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            uint64_t sample16 = edge16 + (3u + 2u * bit) * bit16 / 2u;
            if (level_at(words, count, sample16 / 16u)) value |= (uint8_t)(1u << bit);
        }
        uint64_t stop16 = edge16 + 19u * bit16 / 2u;
        values[decoded] = value;
        stop_ok[decoded] = level_at(words, count, stop16 / 16u) == 1;
        decoded++;
        earliest16 = stop16;
    }
    return decoded;
}
