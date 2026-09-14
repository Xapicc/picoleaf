#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decodes 8N1 UART (idle high, LSB first) from an edge_capture recording.
//
// `words` is laid out as the PIO edge recorder pushes it: words[0] is the start
// level (0 or 1), words[1..count) are countdown timestamps taken at each level
// change. Timestamps must not wrap within the recording (~68 s at 62.5 MHz).
// Returns the number of bytes written to `values` / `stop_ok`.
size_t uart_decode(const uint32_t *words, size_t count, uint32_t tick_hz, uint32_t baud, uint8_t *values,
                   bool *stop_ok, size_t max);
