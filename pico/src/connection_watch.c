#include "connection_watch.h"

#include <string.h>

void connection_watch_init(connection_watch_t *watch, uint64_t now_us) {
    memset(watch, 0, sizeof *watch);
    watch->down_since_us = now_us;
}

connection_signal_t connection_watch_update(connection_watch_t *watch, bool connected, uint64_t now_us) {
    if (connected && !watch->connected) {
        bool worth_signalling = !watch->ever_connected || watch->problem_signalled;
        watch->connected = watch->ever_connected = true;
        watch->problem_signalled = false;
        return worth_signalling ? CONNECTION_SIGNAL_OK : CONNECTION_SIGNAL_NONE;
    }
    if (!connected && watch->connected) {
        watch->connected = false;
        watch->down_since_us = now_us;
        return CONNECTION_SIGNAL_NONE;
    }
    if (!connected && !watch->problem_signalled && now_us >= watch->down_since_us + CONNECTION_PROBLEM_AFTER_US) {
        watch->problem_signalled = true;
        return CONNECTION_SIGNAL_PROBLEM;
    }
    return CONNECTION_SIGNAL_NONE;
}
