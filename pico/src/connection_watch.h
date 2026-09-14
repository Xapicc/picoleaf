#pragma once

// Decides when the wall should signal the MQTT connection: a problem once it has been down for
// CONNECTION_PROBLEM_AFTER_US (counted from boot for the first connection), and success on the
// first connection and on recovering from a signalled problem. Short outages signal nothing, so a
// broker restart at night doesn't flash the wall. Pure C so the host tests can run it.

#include <stdbool.h>
#include <stdint.h>

#define CONNECTION_PROBLEM_AFTER_US 30000000u

typedef enum {
    CONNECTION_SIGNAL_NONE,
    CONNECTION_SIGNAL_OK,
    CONNECTION_SIGNAL_PROBLEM,
} connection_signal_t;

typedef struct {
    bool connected;
    bool ever_connected;
    bool problem_signalled;
    uint64_t down_since_us;
} connection_watch_t;

void connection_watch_init(connection_watch_t *watch, uint64_t now_us);

// Call every main-loop pass; returns a signal at most once per change.
connection_signal_t connection_watch_update(connection_watch_t *watch, bool connected, uint64_t now_us);
