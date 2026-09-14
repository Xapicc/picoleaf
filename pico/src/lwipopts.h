#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// lwIP for pico_cyw43_arch_lwip_poll: no OS, everything runs from the main loop.
// Based on the pico-examples defaults, sized down to leave RAM for the bus recorder.

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
// TCP copies every MQTT publish into this heap; 8 KB stalled the client after ~4 KB of discovery.
#define MEM_SIZE 24000
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
#define PBUF_POOL_SIZE 24
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_TCP_KEEPALIVE 1
#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0

// The MQTT client needs one extra timeout for its keep-alive timer.
#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1)
// Discovery messages are ~500 bytes each and are queued back to back after connecting.
#define MQTT_OUTPUT_RINGBUF_SIZE 4096
#define MQTT_VAR_HEADER_BUFFER_LEN 512
#define MQTT_REQ_MAX_IN_FLIGHT 8

#endif
