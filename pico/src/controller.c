#include "controller.h"

#include <string.h>

#define SESSION_RETRY_US 1000000u
// Relayed replies finish within ~0.6 ms for 13 squares (docs/panel-bus.md).
#define LAYOUT_LISTEN_MS 20u
#define POLL_LISTEN_MS 5u
#define LAYOUT_END 0x40
#define SQUARE_HEADER 0xC0
#define REPLY_MAX (2 * CONTROLLER_MAX_SQUARES)
#define UID_LISTEN_MS 10u
#define UID_READ_ATTEMPTS 5u
#define UID_REPLY_OK 0x01

void controller_init(controller_t *controller, uint64_t now_us) {
    memset(controller, 0, sizeof *controller);
    controller->enabled = true;
    controller->brightness = 0xFF;
    controller->next_session_attempt_us = now_us;
    controller->next_tick_us = now_us;
}

const uint8_t *controller_uid(const controller_t *controller, size_t index) {
    return controller->uids[index];
}

uint16_t controller_crc16_arc(const uint8_t *data, size_t length) {
    uint16_t crc = 0;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

size_t controller_count_squares(const uint8_t *layout, size_t length) {
    size_t squares = 0;
    for (size_t i = 0; i < length; i++) {
        if ((layout[i] & 0xF0) == SQUARE_HEADER) squares++;
    }
    return squares;
}

size_t controller_colour_frame(const controller_t *controller, uint8_t *frame, size_t max) {
    size_t length = 2 + 6 * controller->square_count;
    if (length > max) return 0;
    frame[0] = 0xE0;
    frame[1] = 0x01;
    size_t offset = 2;
    // Squares take their entries in reverse bus order: the farthest square first.
    for (size_t i = controller->square_count; i-- > 0;) {
        frame[offset++] = 0x05;
        frame[offset++] = 0x00;  // transition: instant
        memcpy(frame + offset, controller->colours[i], 4);
        offset += 4;
    }
    return length;
}

bool controller_set(controller_t *controller, size_t index, const uint8_t rgbw[4]) {
    if (index >= CONTROLLER_MAX_SQUARES) return false;
    memcpy(controller->colours[index], rgbw, 4);
    controller->colours_set = true;
    return true;
}

void controller_fill(controller_t *controller, const uint8_t rgbw[4]) {
    for (size_t i = 0; i < CONTROLLER_MAX_SQUARES; i++) memcpy(controller->colours[i], rgbw, 4);
    controller->colours_set = true;
}

void controller_set_brightness(controller_t *controller, uint8_t brightness) {
    controller->brightness = brightness;
    controller->brightness_set = true;
}

// Reply to `F8 <index LE> 82`: one 00 per relay hop, 01, the 16-byte ID, CRC-16/ARC little-endian.
// The hop count is the square's depth in the layout tree, not its index, so any number of 00s is accepted.
static bool read_uid(controller_t *controller, size_t index, controller_exchange_fn exchange) {
    const uint8_t frame[] = {0xF8, (uint8_t)index, (uint8_t)(index >> 8), 0x82};
    uint8_t reply[CONTROLLER_MAX_SQUARES + 1 + CONTROLLER_UID_LENGTH + 2];
    size_t length = exchange(frame, sizeof frame, UID_LISTEN_MS, reply, sizeof reply);
    size_t cursor = 0;
    while (cursor < length && reply[cursor] == 0x00) cursor++;
    if (cursor + 1 + CONTROLLER_UID_LENGTH + 2 > length || reply[cursor] != UID_REPLY_OK) {
        controller->stats.uid_read_failures++;
        return false;
    }
    const uint8_t *uid = reply + cursor + 1;
    uint16_t crc = (uint16_t)(uid[CONTROLLER_UID_LENGTH] | uid[CONTROLLER_UID_LENGTH + 1] << 8);
    if (controller_crc16_arc(uid, CONTROLLER_UID_LENGTH) != crc) {
        controller->stats.uid_read_failures++;
        return false;
    }
    memcpy(controller->uids[index], uid, CONTROLLER_UID_LENGTH);
    return true;
}

static controller_event_t open_session(controller_t *controller, uint64_t now_us, controller_exchange_fn exchange) {
    uint8_t ignored[4];
    exchange((const uint8_t[]){0x00}, 1, 2, ignored, sizeof ignored);
    size_t length =
        exchange((const uint8_t[]){0x80}, 1, LAYOUT_LISTEN_MS, controller->layout, sizeof controller->layout);
    size_t squares = controller_count_squares(controller->layout, length);
    if (length == 0 || controller->layout[length - 1] != LAYOUT_END || squares == 0 ||
        squares > CONTROLLER_MAX_SQUARES) {
        controller->layout_length = 0;
        controller->stats.session_failures++;
        controller->next_session_attempt_us = now_us + SESSION_RETRY_US;
        return CONTROLLER_SESSION_FAILED;
    }
    controller->layout_length = length;
    controller->square_count = squares;
    controller->uids_read = 0;
    controller->uid_attempts = 0;
    controller->in_session = true;
    controller->last_good_poll_us = now_us;
    controller->next_tick_us = now_us;
    controller->stats.sessions_opened++;
    return CONTROLLER_SESSION_OPENED;
}

controller_event_t controller_tick(controller_t *controller, uint64_t now_us, controller_exchange_fn exchange) {
    if (!controller->enabled) return CONTROLLER_IDLE;
    if (!controller->in_session) {
        if (now_us < controller->next_session_attempt_us) return CONTROLLER_IDLE;
        return open_session(controller, now_us, exchange);
    }
    if (now_us < controller->next_tick_us) return CONTROLLER_IDLE;

    uint8_t reply[REPLY_MAX];
    bool uids_completed = false;
    if (controller->uids_read < controller->square_count) {
        if (read_uid(controller, controller->uids_read, exchange)) {
            controller->uids_read++;
            controller->uid_attempts = 0;
            uids_completed = controller->uids_read == controller->square_count;
        } else if (++controller->uid_attempts >= UID_READ_ATTEMPTS) {
            // A square that never answers its ID read: start over with a fresh layout.
            controller->uid_attempts = 0;
            controller->in_session = false;
            controller->next_session_attempt_us = now_us + SESSION_RETRY_US;
            controller->stats.sessions_lost++;
            return CONTROLLER_SESSION_LOST;
        }
    }
    // Squares power up at about half global brightness and keep FC 04 only until their power is cut,
    // so it goes out with every colour frame; otherwise the RGB scaling from Home Assistant tops out dim.
    if (controller->brightness_set || controller->colours_set) {
        exchange((const uint8_t[]){0xFC, 0x04, controller->brightness}, 3, 0, reply, sizeof reply);
    }
    if (controller->colours_set) {
        uint8_t frame[2 + 6 * CONTROLLER_MAX_SQUARES];
        size_t length = controller_colour_frame(controller, frame, sizeof frame);
        exchange(frame, length, 0, reply, sizeof reply);
        controller->stats.frames_sent++;
    }

    size_t poll_length = exchange((const uint8_t[]){0xC0}, 1, POLL_LISTEN_MS, reply, sizeof reply);
    controller->next_tick_us = now_us + CONTROLLER_TICK_US;
    if (poll_length != 2 * controller->square_count) {
        // A CC reply, silence, or a changed square count: re-read the layout.
        controller->in_session = false;
        controller->next_session_attempt_us = now_us;
        controller->stats.sessions_lost++;
        controller->stats.last_bad_poll_length = (uint32_t)poll_length;
        return CONTROLLER_SESSION_LOST;
    }
    uint64_t gap = now_us - controller->last_good_poll_us;
    if (gap > controller->stats.max_poll_gap_us) controller->stats.max_poll_gap_us = gap;
    controller->last_good_poll_us = now_us;
    controller->stats.polls_ok++;
    return uids_completed ? CONTROLLER_UIDS_READY : CONTROLLER_IDLE;
}
