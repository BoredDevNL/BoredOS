// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "kernel_subsystem.h"
#include "network.h"
#include "memory_manager.h"
#include "kutils.h"

// Helpers to format IPs and MACs
static void format_ip(const ipv4_address_t *ip, char *out) {
  char temp[16];
  out[0] = 0;
  for (int i = 0; i < 4; i++) {
    itoa(ip->bytes[i], temp);
    strcpy(out + strlen(out), temp);
    if (i < 3) strcpy(out + strlen(out), ".");
  }
  strcpy(out + strlen(out), "\n");
}

static void format_mac(const mac_address_t *mac, char *out) {
  char temp[8];
  out[0] = 0;
  for (int i = 0; i < 6; i++) {
    itoa_hex(mac->bytes[i], temp);
    if (strlen(temp) == 1) {
      strcpy(out + strlen(out), "0");
    }
    strcpy(out + strlen(out), temp);
    if (i < 5) strcpy(out + strlen(out), ":");
  }
  strcpy(out + strlen(out), "\n");
}

// IP/MAC parsing
static int parse_ip(const char *str, ipv4_address_t *ip) {
  int val = 0;
  int part = 0;
  const char *p = str;
  while (*p && *p != '\n' && *p != ' ' && *p != '\r') {
    if (*p >= '0' && *p <= '9') {
      val = val * 10 + (*p - '0');
      if (val > 255) return -1;
    } else if (*p == '.') {
      if (part > 3) return -1;
      ip->bytes[part++] = (uint8_t)val;
      val = 0;
    } else {
      return -1;
    }
    p++;
  }
  if (part != 3) return -1;
  ip->bytes[3] = (uint8_t)val;
  return 0;
}

// Subsystem file handlers
static int read_net_address(char *buf, int size, int offset) {
  mac_address_t mac;
  if (network_get_mac_address(&mac) == 0) {
    char out[64];
    format_mac(&mac, out);
    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
  }
  return -1;
}

static int read_net_ip(char *buf, int size, int offset) {
  ipv4_address_t ip;
  if (network_get_ipv4_address(&ip) == 0) {
    char out[64];
    format_ip(&ip, out);
    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
  }
  return -1;
}

static int read_net_gateway(char *buf, int size, int offset) {
  ipv4_address_t ip;
  if (network_get_gateway_ip(&ip) == 0) {
    char out[64];
    format_ip(&ip, out);
    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
  }
  return -1;
}

static int read_net_dns(char *buf, int size, int offset) {
  ipv4_address_t ip;
  if (network_get_dns_ip(&ip) == 0) {
    char out[64];
    format_ip(&ip, out);
    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
  }
  return -1;
}

static int read_net_nic(char *buf, int size, int offset) {
  char out[64];
  if (network_get_nic_name(out) == 0) {
    strcpy(out + strlen(out), "\n");
    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
  }
  return -1;
}

static int read_net_status(char *buf, int size, int offset) {
  char out[128];
  out[0] = 0;
  strcpy(out, "initialized: ");
  strcpy(out + strlen(out), network_is_initialized() ? "1\n" : "0\n");
  strcpy(out + strlen(out), "has_ip: ");
  strcpy(out + strlen(out), network_has_ip() ? "1\n" : "0\n");
  
  int len = strlen(out);
  if (offset >= len) return 0;
  int to_copy = len - offset;
  if (to_copy > size) to_copy = size;
  memcpy(buf, out + offset, to_copy);
  return to_copy;
}

static int read_net_stats(char *buf, int size, int offset) {
  char out[256];
  out[0] = 0;
  char s[32];
  
  strcpy(out, "rx_frames: ");
  itoa(network_get_frames_received(), s);
  strcpy(out + strlen(out), s);
  
  strcpy(out + strlen(out), "\ntx_frames: ");
  itoa(network_get_frames_sent(), s);
  strcpy(out + strlen(out), s);
  
  strcpy(out + strlen(out), "\nrx_udp: ");
  itoa(network_get_udp_packets_received(), s);
  strcpy(out + strlen(out), s);
  strcpy(out + strlen(out), "\n");

  int len = strlen(out);
  if (offset >= len) return 0;
  int to_copy = len - offset;
  if (to_copy > size) to_copy = size;
  memcpy(buf, out + offset, to_copy);
  return to_copy;
}

static int write_net_control(const char *buf, int size, int offset) {
  (void)offset;
  if (strncmp(buf, "dhcp", 4) == 0) {
    return network_dhcp_acquire() == 0 ? size : -1;
  } else if (strncmp(buf, "init", 4) == 0) {
    return network_init() == 0 ? size : -1;
  } else if (strncmp(buf, "set_dns ", 8) == 0) {
    ipv4_address_t ip;
    if (parse_ip(buf + 8, &ip) == 0) {
      return network_set_dns_server(&ip) == 0 ? size : -1;
    }
  } else if (strncmp(buf, "set_ip ", 7) == 0) {
    ipv4_address_t ip;
    if (parse_ip(buf + 7, &ip) == 0) {
      return network_set_ipv4_address(&ip) == 0 ? size : -1;
    }
  }
  return -1;
}

void sysfs_net_init(void) {
  kernel_subsystem_t *net_sub;
  subsystem_register("class/net/eth0", &net_sub);
  subsystem_add_file(net_sub, "address", read_net_address, NULL);
  subsystem_add_file(net_sub, "ip", read_net_ip, NULL);
  subsystem_add_file(net_sub, "gateway", read_net_gateway, NULL);
  subsystem_add_file(net_sub, "dns", read_net_dns, NULL);
  subsystem_add_file(net_sub, "nic", read_net_nic, NULL);
  subsystem_add_file(net_sub, "status", read_net_status, NULL);
  subsystem_add_file(net_sub, "stats", read_net_stats, NULL);
  subsystem_add_file(net_sub, "control", NULL, write_net_control);
}
