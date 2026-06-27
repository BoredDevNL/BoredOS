// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stddef.h>
#include "nic.h"

#define ETH_FRAME_MAX_SIZE 1518
#define ETH_HEADER_SIZE 14
#define ETH_ETHERTYPE_ARP  0x0806
#define ETH_ETHERTYPE_IPV4 0x0800

typedef struct { uint8_t bytes[6]; } mac_address_t;
typedef struct { uint8_t bytes[4]; } ipv4_address_t;

typedef struct {
    uint8_t dest_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype;
} __attribute__((packed)) eth_header_t;

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed)) arp_header_t;

#define IP_PROTO_UDP  17

typedef struct {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dest_ip[4];
} __attribute__((packed)) ipv4_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

int network_init(void);
int network_get_mac_address(mac_address_t* mac);
int network_get_nic_name(char* name_out);
int network_get_ipv4_address(ipv4_address_t* ip);
int network_set_ipv4_address(const ipv4_address_t* ip);
int network_send_frame(const void* data, size_t length);
int network_receive_frame(void* buffer, size_t buffer_size);
void network_process_frames(void);
int arp_send_request(const ipv4_address_t* target_ip);
int arp_lookup(const ipv4_address_t* ip, mac_address_t* mac);
void arp_process_packet(const arp_header_t* arp, size_t length);
int ipv4_send_packet(const ipv4_address_t* dest_ip, uint8_t protocol, const void* data, size_t data_length);
int ipv4_send_packet_to_mac(const ipv4_address_t* dest_ip, const mac_address_t* dest_mac, uint8_t protocol, const void* data, size_t data_length);
void ipv4_process_packet(const ipv4_header_t* ip, const mac_address_t* src_mac, size_t length);
int udp_send_packet(const ipv4_address_t* dest_ip, uint16_t dest_port, uint16_t src_port, const void* data, size_t data_length);
int udp_send_packet_to_mac(const ipv4_address_t* dest_ip, const mac_address_t* dest_mac, uint16_t dest_port, uint16_t src_port, const void* data, size_t data_length);
typedef void (*udp_callback_t)(const ipv4_address_t* src_ip, uint16_t src_port, const mac_address_t* src_mac, const void* data, size_t length);
int udp_register_callback(uint16_t port, udp_callback_t callback);
int network_is_initialized(void);
int network_has_ip(void);
int network_get_frames_received(void);
int network_get_frames_sent(void);
int network_get_udp_packets_received(void);
int network_get_udp_callbacks_called(void);
int network_get_e1000_receive_calls(void);
int network_get_e1000_receive_empty(void);
int network_get_process_calls(void);
int network_dhcp_acquire(void);
int network_get_gateway_ip(ipv4_address_t* ip);
int network_get_dns_ip(ipv4_address_t *ip);
int network_icmp_single_ping(ipv4_address_t *dest);
int network_tcp_connect(const ipv4_address_t *ip, uint16_t port);
int network_tcp_listen(uint16_t port);
int network_tcp_accept(void);
int network_tcp_send(const void *data, size_t len);
int network_tcp_recv(void *buf, size_t max_len);
int network_tcp_recv_nb(void *buf, size_t max_len);
int network_tcp_close(void);
int network_dns_lookup(const char *name, ipv4_address_t *out_ip);
int network_set_dns_server(const ipv4_address_t *ip);
int network_socket_bind(void *sock, uint32_t ip_val, uint16_t port);
int network_socket_listen(void *sock);
int network_socket_connect(void *sock, uint32_t ip_val, uint16_t port);
int network_socket_recv(void *sock, void *buf, size_t max_len, int nonblock);
int network_socket_send(void *sock, const void *data, size_t len, int nonblock);
void network_socket_close(void *sock);
void network_socket_get_remote_info(void *sock, uint16_t *port, uint32_t *ip);

#endif
