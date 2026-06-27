// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef NET_DEFS_H
#define NET_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "network.h" 

// Protocol Numbers
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

// Byte Order Utils
static inline uint16_t htons(uint16_t v) {
    return (v << 8) | (v >> 8);
}
static inline uint16_t ntohs(uint16_t v) {
    return (v << 8) | (v >> 8);
}
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}
static inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

static inline uint16_t net_checksum(void *data, int len) {
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)data;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) {
        sum += *(uint8_t *)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

// --- Headers ---

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t q_count;
    uint16_t ans_count;
    uint16_t auth_count;
    uint16_t add_count;
} __attribute__((packed)) dns_header_t;

// --- TCP Socket API ---
typedef struct tcp_socket_t tcp_socket_t; // Opaque type

tcp_socket_t* tcp_connect(ipv4_address_t ip, uint16_t port);
void tcp_send(tcp_socket_t *sock, const char *data, int len);
void tcp_close(tcp_socket_t *sock);
bool tcp_is_connected(tcp_socket_t *sock);
int tcp_read(tcp_socket_t *sock, char *buffer, int max_len);

// --- DNS API ---
ipv4_address_t dns_resolve(const char *hostname);

// External dependencies from your existing IP layer
// You must ensure these are implemented in network.c
extern int ip_send_packet(ipv4_address_t dst, uint8_t protocol, const void *data, uint16_t len);
extern ipv4_address_t get_local_ip(void);
extern ipv4_address_t get_dns_server_ip(void);

// New functions exposed by modules
void icmp_handle_packet(ipv4_address_t src, void *data, uint16_t len);
void tcp_handle_packet(ipv4_address_t src, void *data, uint16_t len);

// CLI Commands
void cli_cmd_ping(char *args);
void cli_cmd_dns(char *args);
void cli_cmd_httpget(char *args);

#endif
