#include "layout.h"

#include <stdbool.h>

#define EMPTY 0x00
#define END 0x40
#define SQUARE 0xC0
#define PSU 0x90
#define READ_ENDED (-1)
#define READ_ERROR (-2)

// Clockwise as seen from the front: down, left, up, right.
static const int8_t STEP_X[4] = {0, -1, 0, 1};
static const int8_t STEP_Y[4] = {-1, 0, 1, 0};

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t position;
    bool ended;
    layout_position_t *positions;
    size_t max;
    size_t count;
    bool failed;
} parser_t;

static int next_byte(parser_t *parser) {
    if (parser->ended) return READ_ENDED;
    if (parser->position >= parser->length) {
        parser->failed = true;  // no end marker
        return READ_ERROR;
    }
    uint8_t value = parser->data[parser->position++];
    if (value == END) {
        parser->ended = true;
        return READ_ENDED;
    }
    return value;
}

static void read_square(parser_t *parser, int x, int y, int entry_direction) {
    if (parser->count >= parser->max || x < INT8_MIN || x > INT8_MAX || y < INT8_MIN || y > INT8_MAX) {
        parser->failed = true;
        return;
    }
    for (size_t i = 0; i < parser->count; i++) {
        if (parser->positions[i].x == x && parser->positions[i].y == y) {
            parser->failed = true;  // the model placed two squares in one spot
            return;
        }
    }
    parser->positions[parser->count++] = (layout_position_t){(int8_t)x, (int8_t)y};

    for (int turn = 1; turn <= 3 && !parser->failed; turn++) {
        int direction = (entry_direction + turn) % 4;
        int value = next_byte(parser);
        if (value == READ_ERROR) return;
        if (value == READ_ENDED || value == EMPTY) continue;
        if ((value & 0xF0) == SQUARE) {
            // The neighbour is entered from the side facing back towards this square.
            read_square(parser, x + STEP_X[direction], y + STEP_Y[direction], (direction + 2) % 4);
        } else if ((value & 0xF0) == PSU) {
            if (next_byte(parser) == READ_ERROR) return;  // trailing byte, 00 on every reading so far
        } else {
            parser->failed = true;
        }
    }
}

int layout_positions(const uint8_t *reply, size_t length, layout_position_t *positions, size_t max) {
    parser_t parser = {.data = reply, .length = length, .positions = positions, .max = max};
    int header = next_byte(&parser);
    if (header < 0 || (header & 0xF0) != SQUARE) return -1;
    read_square(&parser, 0, 0, header & 0x0F);
    if (parser.failed || parser.position != length) return -1;
    return (int)parser.count;
}
