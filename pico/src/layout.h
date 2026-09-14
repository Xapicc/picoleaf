#pragma once

// Places squares on a grid from the layout reply (the answer to `00` then `80`).
// C port of tools/layout.py; the encoding is described in docs/panel-bus.md, "Layout encoding".
// Positions are in the first square's frame: (0, 0) is the square the controller is plugged
// into, x grows to its right and y upwards. Squares are numbered in reply order, which is the
// bus order used for colour frames and F8 addressing (verified on the 13-square wall).

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int8_t x;
    int8_t y;
} layout_position_t;

// Returns the number of squares placed, or -1 if the reply doesn't fit the encoding
// (unknown node byte, missing end marker, leftover bytes, overlapping squares, more than `max`).
int layout_positions(const uint8_t *reply, size_t length, layout_position_t *positions, size_t max);
