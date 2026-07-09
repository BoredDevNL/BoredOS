// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "kernel_subsystem.h"
#include "process.h"
#include "network.h"
#include "memory_manager.h"
#include "kutils.h"
#include <limits.h>

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
static int read_net_address(char *buf, size_t size, size_t offset) {
  mac_address_t mac;
  if (network_get_mac_address(&mac) == 0) {
    char out[64];
    format_mac(&mac, out);
    size_t len = strlen(out);
    if (offset >= len) return 0;
    size_t to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    if (to_copy > INT_MAX) return 0;
    memcpy(buf, out + offset, to_copy);
    return (int)to_copy;
  }
  return -1;
}

static int read_ip_field(char *buf, size_t size, size_t offset,
                         int (*getter)(ipv4_address_t *)) {
  ipv4_address_t ip;
  if (getter(&ip) == 0) {
    char out[64];
    format_ip(&ip, out);
    size_t len = strlen(out);
    if (offset >= len) return 0;
    size_t to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    if (to_copy > INT_MAX) return 0;
    memcpy(buf, out + offset, to_copy);
    return (int)to_copy;
  }
  return -1;
}

static int read_net_ip(char *buf, size_t size, size_t offset) {
  return read_ip_field(buf, size, offset, network_get_ipv4_address);
}

static int read_net_gateway(char *buf, size_t size, size_t offset) {
  return read_ip_field(buf, size, offset, network_get_gateway_ip);
}


static int read_net_nic(char *buf, size_t size, size_t offset) {
  char out[64];
  if (network_get_nic_name(out) == 0) {
    strcpy(out + strlen(out), "\n");
    size_t len = strlen(out);
    if (offset >= len) return 0;
    size_t to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    if (to_copy > INT_MAX) return 0;
    memcpy(buf, out + offset, to_copy);
    return (int)to_copy;
  }
  return -1;
}

static int read_net_status(char *buf, size_t  size, size_t offset) {
  char out[128];
  out[0] = 0;
  strcpy(out, "initialized: ");
  strcpy(out + strlen(out), network_is_initialized() ? "1\n" : "0\n");
  strcpy(out + strlen(out), "has_ip: ");
  strcpy(out + strlen(out), network_has_ip() ? "1\n" : "0\n");
  
  size_t len = strlen(out);
  if (offset >= len) return 0;
  size_t to_copy = len - offset;
  if (to_copy > size) to_copy = size;
  if (to_copy > INT_MAX) return 0;
  memcpy(buf, out + offset, to_copy);
  return (int)to_copy;
}

static int read_net_stats(char *buf, size_t size, size_t offset) {
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

  size_t len = strlen(out);
  if (offset >= len) return 0;
  size_t to_copy = len - offset;
  if (to_copy > size) to_copy = size;
  if (to_copy > INT_MAX) return 0;
  memcpy(buf, out + offset, to_copy);
  return (int)to_copy;
}

static int write_net_control(const char *buf, size_t size, size_t offset) {
  (void)offset;
  if (strncmp(buf, "dhcp", 4) == 0) {
    return network_dhcp_acquire() == 0 ? (int)size : -1;
  } else if (strncmp(buf, "init", 4) == 0) {
    return network_init() == 0 ? (int)size : -1;
  } else if (strncmp(buf, "set_ip ", 7) == 0) {
    ipv4_address_t ip;
    if (parse_ip(buf + 7, &ip) == 0) {
      return network_set_ipv4_address(&ip) == 0 ? (int)size : -1;
    }
  }
  return -1;
}



static int write_ping(const char *buf, size_t size, size_t offset) {
  (void)offset;
  process_t *proc = process_get_current();
  if (!proc) return -1;
  char target[64];
  size_t len = size < 63 ? size : 63;
  memcpy(target, buf, len);
  target[len] = '\0';
  while (len > 0 && (target[len - 1] == '\n' || target[len - 1] == '\r' || target[len - 1] == ' ')) {
    target[--len] = '\0';
  }
  
  ipv4_address_t ip;
  if (parse_ip(target, &ip) == 0) {
    extern int network_icmp_single_ping(ipv4_address_t *ip);
    int rc = network_icmp_single_ping(&ip);
    if (rc >= 0) {
      char rtt_str[32];
      itoa(rc, rtt_str);
      char *r = proc->ping_result;
      strcpy(r, "success ");
      strcpy(r + strlen(r), rtt_str);
      strcpy(r + strlen(r), "\n");
    } else {
      strcpy(proc->ping_result, "failed\n");
    }
  } else {
    strcpy(proc->ping_result, "invalid\n");
  }
  return size;
}

static int read_ping(char *buf, size_t size, size_t offset) {
  process_t *proc = process_get_current();
  if (!proc) return -1;
  size_t len = strlen(proc->ping_result);
  if (offset >= len) return 0;
  size_t to_copy = len - offset;
  if (to_copy > size) to_copy = size;
  if (to_copy > INT_MAX) return 0;
  memcpy(buf, proc->ping_result + offset, to_copy);
  return (int)to_copy;
}

void sysfs_net_init(void) {
  char path[64];
  extern const char* nic_get_active_name(void);
  const char* active_name = nic_get_active_name();
  if (active_name) {
    strcpy(path, "class/net/");
    strcat(path, active_name);
  } else {
    strcpy(path, "class/net/eth0");
  }

  kernel_subsystem_t *net_sub;
  subsystem_register(path, &net_sub);
  subsystem_add_file(net_sub, "address", read_net_address, NULL);
  subsystem_add_file(net_sub, "ip", read_net_ip, NULL);
  subsystem_add_file(net_sub, "gateway", read_net_gateway, NULL);
  subsystem_add_file(net_sub, "nic", read_net_nic, NULL);
  subsystem_add_file(net_sub, "status", read_net_status, NULL);
  subsystem_add_file(net_sub, "stats", read_net_stats, NULL);
  subsystem_add_file(net_sub, "control", NULL, write_net_control);
  subsystem_add_file(net_sub, "ping", read_ping, write_ping);
}
