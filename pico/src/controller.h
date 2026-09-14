#pragma once

// Canvas panel controller: keeps a session with the squares, polls them, and
// pushes per-square colours. Pure C with the bus injected, so it can be tested
// on the host. Protocol and timings: docs/panel-bus.md.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONTROLLER_MAX_SQUARES 64
#define CONTROLLER_LAYOUT_MAX 512
#define CONTROLLER_UID_LENGTH 16
#define CONTROLLER_TICK_US 40000u  // 25 Hz colour frames and polls

// Sends `length` bytes, records for `listen_ms` afterwards, and returns how many
// reply bytes (after our own frame) were written to `reply`.
typedef size_t (*controller_exchange_fn)(const uint8_t *frame, size_t length, uint32_t listen_ms, uint8_t *reply,
                                         size_t reply_max);

typedef enum {
    CONTROLLER_IDLE,
    CONTROLLER_SESSION_OPENED,
    CONTROLLER_SESSION_FAILED,
    CONTROLLER_SESSION_LOST,
    // Every square's hardware ID has been read since the session opened.
    CONTROLLER_UIDS_READY,
} controller_event_t;

typedef struct {
    uint32_t sessions_opened;
    uint32_t session_failures;
    uint32_t sessions_lost;
    uint32_t frames_sent;
    uint32_t polls_ok;
    uint64_t max_poll_gap_us;
    // Reply length of the poll that last ended a session (0 = silence, 1 = CC).
    uint32_t last_bad_poll_length;
    uint32_t uid_read_failures;
} controller_stats_t;

typedef struct {
    bool enabled;
    bool in_session;
    // Nothing is pushed until a colour or brightness has been set, so squares
    // keep their own default look after power-up.
    bool colours_set;
    bool brightness_set;
    uint8_t brightness;
    size_t square_count;
    uint8_t colours[CONTROLLER_MAX_SQUARES][4];  // RGBW, indexed in bus order
    // Hardware IDs, read one per tick after the session opens (`F8 <index> 82`).
    uint8_t uids[CONTROLLER_MAX_SQUARES][CONTROLLER_UID_LENGTH];
    size_t uids_read;
    unsigned uid_attempts;
    uint8_t layout[CONTROLLER_LAYOUT_MAX];
    size_t layout_length;
    uint64_t next_tick_us;
    uint64_t next_session_attempt_us;
    uint64_t last_good_poll_us;
    controller_stats_t stats;
} controller_t;

void controller_init(controller_t *controller, uint64_t now_us);

controller_event_t controller_tick(controller_t *controller, uint64_t now_us, controller_exchange_fn exchange);

// Index is in bus order (0 = the square the controller is plugged into). Returns false if out of range.
bool controller_set(controller_t *controller, size_t index, const uint8_t rgbw[4]);
void controller_fill(controller_t *controller, const uint8_t rgbw[4]);
void controller_set_brightness(controller_t *controller, uint8_t brightness);

// The hardware ID of the square at `index` (valid once CONTROLLER_UIDS_READY has been reported).
const uint8_t *controller_uid(const controller_t *controller, size_t index);

// CRC-16/ARC (poly 0x8005 reflected, init 0), as used in addressed-read replies.
uint16_t controller_crc16_arc(const uint8_t *data, size_t length);

// Squares in a layout reply: one 0xC_ header per square.
size_t controller_count_squares(const uint8_t *layout, size_t length);

// `E0 01` followed by one `05 00 R G B W` entry per square, farthest square first.
size_t controller_colour_frame(const controller_t *controller, uint8_t *frame, size_t max);
