#pragma once

// Wi-Fi station and MQTT client for the Pico W, driven from the main loop
// (pico_cyw43_arch_lwip_poll). Reconnects on its own after Wi-Fi or broker loss.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_record.h"

// Called for every message on a subscribed topic, from within net_poll().
typedef void (*net_message_fn)(const char *topic, const char *payload, size_t length);

// `client_id` doubles as the MQTT client id. `subscription` is subscribed to after each connect,
// and `availability_topic` gets a retained "online", with "offline" as the last will.
bool net_init(const device_config_t *config, const char *client_id, const char *subscription,
              const char *availability_topic, net_message_fn on_message);

void net_poll(uint64_t now_us);

bool net_mqtt_connected(void);

// Increments on every successful MQTT connect, so callers know to republish retained state.
uint32_t net_connection_count(void);

// Queues a publish. Returns false if not connected or the client's output buffer is full.
bool net_publish(const char *topic, const char *payload, bool retain);

// One-line human-readable status for the USB `net` command.
void net_describe(char *out, size_t size);
