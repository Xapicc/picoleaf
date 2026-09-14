#include "net.h"

#include <stdio.h>
#include <string.h>

#include "lwip/apps/mqtt.h"
#include "lwip/etharp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"

#define RETRY_DELAY_US 5000000u
#define WIFI_JOIN_TIMEOUT_US 30000000u
#define MQTT_CONNECT_TIMEOUT_US 15000000u
#define MQTT_KEEP_ALIVE_S 30
#define TOPIC_MAX 128
#define PAYLOAD_MAX 512

typedef enum {
    NET_NOT_CONFIGURED,
    NET_WIFI_WAITING,
    NET_WIFI_JOINING,
    NET_MQTT_WAITING,
    NET_MQTT_CONNECTING,
    NET_MQTT_CONNECTED,
} net_state_t;

static const char *STATE_NAMES[] = {"not-configured", "wifi-waiting", "wifi-joining",
                                    "mqtt-waiting",   "mqtt-connecting", "mqtt-connected"};

static struct {
    net_state_t state;
    device_config_t config;
    const char *client_id;
    const char *subscription;
    const char *availability_topic;
    net_message_fn on_message;
    mqtt_client_t *client;
    uint64_t retry_at_us;
    uint64_t attempt_started_us;
    uint32_t connections;
    int last_wifi_status;
    int last_mqtt_status;
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX + 1];
    size_t payload_length;
    bool payload_overflow;
} net;

static void retry_later(net_state_t state) {
    net.state = state;
    net.retry_at_us = time_us_64() + RETRY_DELAY_US;
}

// Docker gives macvlan containers a new random MAC on every restart, so after a broker restart the
// cached ARP entry points at a MAC that no longer exists and every reconnect times out for minutes.
static void forget_link_addresses(void) {
    if (netif_default != NULL) etharp_cleanup_netif(netif_default);
}

static void on_subscribed(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) printf("NET subscribe to %s failed (%d)\n", net.subscription, result);
}

static void on_connection(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    (void)arg;
    net.last_mqtt_status = status;
    if (status != MQTT_CONNECT_ACCEPTED) {
        printf("NET mqtt connection ended (status %d), retrying in 5 s\n", status);
        forget_link_addresses();
        retry_later(NET_MQTT_WAITING);
        return;
    }
    net.state = NET_MQTT_CONNECTED;
    net.connections++;
    printf("NET mqtt connected to %s:%u\n", net.config.mqtt_host, net.config.mqtt_port);
    mqtt_subscribe(client, net.subscription, 0, on_subscribed, NULL);
    mqtt_publish(client, net.availability_topic, "online", 6, 1, 1, NULL, NULL);
}

static void on_incoming_topic(void *arg, const char *topic, u32_t total_length) {
    (void)arg;
    snprintf(net.topic, sizeof net.topic, "%s", topic);
    net.payload_length = 0;
    net.payload_overflow = total_length > PAYLOAD_MAX;
}

static void on_incoming_data(void *arg, const u8_t *data, u16_t length, u8_t flags) {
    (void)arg;
    if (!net.payload_overflow) {
        if (net.payload_length + length > PAYLOAD_MAX) {
            net.payload_overflow = true;
        } else {
            memcpy(net.payload + net.payload_length, data, length);
            net.payload_length += length;
        }
    }
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;
    if (net.payload_overflow) {
        printf("NET dropped oversized message on %s\n", net.topic);
        return;
    }
    net.payload[net.payload_length] = '\0';
    net.on_message(net.topic, net.payload, net.payload_length);
}

bool net_init(const device_config_t *config, const char *client_id, const char *subscription,
              const char *availability_topic, net_message_fn on_message) {
    net.config = *config;
    net.client_id = client_id;
    net.subscription = subscription;
    net.availability_topic = availability_topic;
    net.on_message = on_message;
    if (config->wifi_ssid[0] == '\0' || config->mqtt_host[0] == '\0') {
        net.state = NET_NOT_CONFIGURED;
        return true;
    }
    net.client = mqtt_client_new();
    if (net.client == NULL) return false;
    mqtt_set_inpub_callback(net.client, on_incoming_topic, on_incoming_data, NULL);
    cyw43_arch_enable_sta_mode();
    net.state = NET_WIFI_WAITING;
    net.retry_at_us = 0;
    return true;
}

static void start_wifi_join(uint64_t now_us) {
    int error = cyw43_arch_wifi_connect_async(net.config.wifi_ssid, net.config.wifi_password,
                                              CYW43_AUTH_WPA2_MIXED_PSK);
    if (error != 0) {
        net.last_wifi_status = error;
        retry_later(NET_WIFI_WAITING);
        return;
    }
    net.state = NET_WIFI_JOINING;
    net.attempt_started_us = now_us;
}

static void start_mqtt_connect(uint64_t now_us) {
    ip_addr_t broker;
    if (!ipaddr_aton(net.config.mqtt_host, &broker)) {
        printf("NET mqtt_host '%s' is not an IPv4 address\n", net.config.mqtt_host);
        net.state = NET_NOT_CONFIGURED;
        return;
    }
    struct mqtt_connect_client_info_t info = {
        .client_id = net.client_id,
        .client_user = net.config.mqtt_user[0] ? net.config.mqtt_user : NULL,
        .client_pass = net.config.mqtt_password[0] ? net.config.mqtt_password : NULL,
        .keep_alive = MQTT_KEEP_ALIVE_S,
        .will_topic = net.availability_topic,
        .will_msg = "offline",
        .will_qos = 1,
        .will_retain = 1,
    };
    err_t error = mqtt_client_connect(net.client, &broker, net.config.mqtt_port, on_connection, NULL, &info);
    if (error != ERR_OK) {
        net.last_mqtt_status = error;
        retry_later(NET_MQTT_WAITING);
        return;
    }
    net.state = NET_MQTT_CONNECTING;
    net.attempt_started_us = now_us;
}

void net_poll(uint64_t now_us) {
    if (net.state == NET_NOT_CONFIGURED) return;
    cyw43_arch_poll();

    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    bool link_up = link == CYW43_LINK_UP;
    if (!link_up && net.state >= NET_MQTT_WAITING) {
        printf("NET wifi link lost (%d), rejoining\n", link);
        if (net.state >= NET_MQTT_CONNECTING) mqtt_disconnect(net.client);
        net.state = NET_WIFI_WAITING;
        net.retry_at_us = now_us;
    }

    switch (net.state) {
        case NET_WIFI_WAITING:
            if (now_us >= net.retry_at_us) start_wifi_join(now_us);
            break;
        case NET_WIFI_JOINING:
            net.last_wifi_status = link;
            if (link_up) {
                printf("NET wifi up, address %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
                net.state = NET_MQTT_WAITING;
                net.retry_at_us = now_us;
            } else if (link < 0 || now_us - net.attempt_started_us > WIFI_JOIN_TIMEOUT_US) {
                printf("NET wifi join failed (%d), retrying in 5 s\n", link);
                cyw43_arch_disable_sta_mode();
                cyw43_arch_enable_sta_mode();
                retry_later(NET_WIFI_WAITING);
            }
            break;
        case NET_MQTT_WAITING:
            if (now_us >= net.retry_at_us) start_mqtt_connect(now_us);
            break;
        case NET_MQTT_CONNECTING:
            if (now_us - net.attempt_started_us > MQTT_CONNECT_TIMEOUT_US) {
                printf("NET mqtt connect timed out, retrying in 5 s\n");
                mqtt_disconnect(net.client);
                forget_link_addresses();
                retry_later(NET_MQTT_WAITING);
            }
            break;
        default:
            break;
    }
}

bool net_mqtt_connected(void) {
    return net.state == NET_MQTT_CONNECTED && mqtt_client_is_connected(net.client);
}

uint32_t net_connection_count(void) {
    return net.connections;
}

bool net_publish(const char *topic, const char *payload, bool retain) {
    if (!net_mqtt_connected()) return false;
    size_t length = strlen(payload);
    if (length > 0xFFFF) return false;
    return mqtt_publish(net.client, topic, payload, (u16_t)length, 0, retain ? 1 : 0, NULL, NULL) == ERR_OK;
}

void net_describe(char *out, size_t size) {
    const char *address = "-";
    if (net.state >= NET_MQTT_WAITING && netif_default != NULL) address = ip4addr_ntoa(netif_ip4_addr(netif_default));
    snprintf(out, size, "state=%s ssid=%s address=%s broker=%s:%u wifi_status=%d mqtt_status=%d connections=%lu",
             STATE_NAMES[net.state], net.config.wifi_ssid[0] ? net.config.wifi_ssid : "-", address,
             net.config.mqtt_host[0] ? net.config.mqtt_host : "-", net.config.mqtt_port, net.last_wifi_status,
             net.last_mqtt_status, (unsigned long)net.connections);
}
