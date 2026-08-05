#include "network.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/stats.h"
#include "lwip/raw.h"
#include "lwip/sys.h"
#include "netif/ethernet.h"
#include "nic_netif.h"
#include "kutils.h"
#include "pci.h"
#include "e1000.h"
#include "nic.h"
#include "spinlock.h"
#include "process.h"

#define SO_REUSEADDR    2
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_REUSEPORT   15
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_BROADCAST    6
#define TCP_NODELAY     1

static struct netif nic_netif;
static int lwip_initialized = 0;


typedef struct udp_packet {
    struct pbuf *p;
    ip_addr_t addr;
    u16_t port;
    struct udp_packet *next;
} udp_packet_t;

static void udp_socket_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    process_fd_socket_t *sock = (process_fd_socket_t *)arg;
    if (!sock || !p) return;

    sockbuf_append(&sock->rx_sb, p, addr, port);
    pbuf_free(p);
    wait_queue_wake_all(&sock->rx_waitq);
} 


static wait_queue_head_t net_rx_waitq;
static volatile int net_rx_pending = 0;

static void net_rx_irq_notify(void) {
    net_rx_pending = 1;
    wait_queue_wake_all(&net_rx_waitq);
}

static void net_worker_loop(void) {
    wait_queue_init(&net_rx_waitq);

    extern void e1000_set_rx_notify(void (*cb)(void));
    e1000_set_rx_notify(net_rx_irq_notify);

    while (1) {
        net_rx_pending = 0;
        int count = network_process_frames();
        if (count == 0 && !net_rx_pending) {
            wait_queue_wait_timeout(&net_rx_waitq, 5);
        }
    }
}

int network_init(void) {
    if (lwip_initialized) return 0;
    
    // First, find and initialize the generic NIC device if not already done
    if (nic_init() != 0) {
        return -1; // No supported NIC found
    }

    lwip_init();
#if LWIP_DNS
    dns_init(); // Explicitly init DNS just in case
#endif
    
    ip4_addr_t ipaddr, netmask, gw;
    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);
    
    if (netif_add(&nic_netif, &ipaddr, &netmask, &gw, NULL, nic_netif_init, ethernet_input) == NULL) {
        return -1;
    }
    
    netif_set_default(&nic_netif);
    netif_set_up(&nic_netif);
    
    lwip_initialized = 1;

    extern process_t* process_create(void (*entry_point)(void), bool is_user);
    process_create(net_worker_loop, false);

    extern void serial_write(const char *str);
    serial_write("[NET] Network interface initialized and background net thread spawned\n");

    return 0;
}

int network_get_mac_address(mac_address_t* mac) {
    if (!lwip_initialized) return -1;
    return nic_get_mac_address(mac->bytes);
}

int network_get_nic_name(char* name_out) {
    extern const char* nic_get_active_name(void);
    const char* n = nic_get_active_name();
    if (!n) {
        if (name_out) name_out[0] = 0;
        return -1;
    }
    while (*n) *name_out++ = *n++;
    *name_out = 0;
    return 0;
}

int network_get_ipv4_address(ipv4_address_t* ip) {
    if (!lwip_initialized) {
        ip->bytes[0] = 0; ip->bytes[1] = 0; ip->bytes[2] = 0; ip->bytes[3] = 0;
        return 0;
    }
    u32_t addr = ip4_addr_get_u32(netif_ip4_addr(&nic_netif));
    ip->bytes[0] = (addr >> 0) & 0xFF;
    ip->bytes[1] = (addr >> 8) & 0xFF;
    ip->bytes[2] = (addr >> 16) & 0xFF;
    ip->bytes[3] = (addr >> 24) & 0xFF;
    return 0;
}

int network_set_ipv4_address(const ipv4_address_t* ip) {
    if (!lwip_initialized) return -1;
    ip4_addr_t ipaddr, netmask;
    IP4_ADDR(&ipaddr, ip->bytes[0], ip->bytes[1], ip->bytes[2], ip->bytes[3]);
    netif_set_ipaddr(&nic_netif, &ipaddr);

    if (ip4_addr_isany_val(*netif_ip4_netmask(&nic_netif))) {
        IP4_ADDR(&netmask, 255, 255, 255, 0);
        netif_set_netmask(&nic_netif, &netmask);
    }
    return 0;
}

int network_set_netmask(const ipv4_address_t* mask) {
    if (!lwip_initialized || !mask) return -1;
    ip4_addr_t mask_addr;
    IP4_ADDR(&mask_addr, mask->bytes[0], mask->bytes[1], mask->bytes[2], mask->bytes[3]);
    netif_set_netmask(&nic_netif, &mask_addr);
    return 0;
}

static spinlock_t network_lock = SPINLOCK_INIT;

int network_process_frames(void) {
    if (!lwip_initialized) return 0;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    int count = nic_netif_poll(&nic_netif);
    netif_poll_all();
    sys_check_timeouts();
    spinlock_release_irqrestore(&network_lock, flags);
    return count;
}



int network_dhcp_acquire(void) {
    // DHCP has been moved to userspace (dhclient.elf)
    return -1;
}

int network_get_gateway_ip(ipv4_address_t *ip) {
    if (!lwip_initialized) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    u32_t addr = ip4_addr_get_u32(netif_ip4_gw(&nic_netif));
    ip->bytes[0] = (addr >> 0) & 0xFF;
    ip->bytes[1] = (addr >> 8) & 0xFF;
    ip->bytes[2] = (addr >> 16) & 0xFF;
    ip->bytes[3] = (addr >> 24) & 0xFF;
    spinlock_release_irqrestore(&network_lock, flags);
    return 0;
}

int network_set_gateway_ip(const ipv4_address_t *ip) {
    if (!lwip_initialized || !ip) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    ip4_addr_t gw_addr;
    IP4_ADDR(&gw_addr, ip->bytes[0], ip->bytes[1], ip->bytes[2], ip->bytes[3]);
    netif_set_gw(&nic_netif, &gw_addr);
    spinlock_release_irqrestore(&network_lock, flags);
    return 0;
}

int network_is_initialized(void) { return lwip_initialized; }
int network_has_ip(void) { return lwip_initialized && !ip4_addr_isany_val(*netif_ip4_addr(&nic_netif)); }

int network_send_frame(const void* data, size_t length) { return nic_send_packet(data, length); }
int network_receive_frame(void* buffer, size_t buffer_size) { return nic_receive_packet(buffer, buffer_size); }

static u16_t icmp_cksum(void *data, int len) {
    u32_t sum = 0;
    u16_t *p = (u16_t *)data;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(u8_t *)p;
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (u16_t)(~sum);
}

int udp_send_packet(const ipv4_address_t* dest_ip, uint16_t dest_port, uint16_t src_port, const void* data, size_t data_length) {
    (void)dest_ip; (void)dest_port; (void)src_port; (void)data; (void)data_length;
    return -1; 
}

typedef struct {
    volatile int done;
    volatile uint32_t start;
} ping_state_t;

static u16_t ping_seq = 0;

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)pcb; (void)addr;
    ping_state_t *state = (ping_state_t *)arg;

    if (state && !state->done && p->tot_len >= 8) {
        u8_t *data = (u8_t *)p->payload;
        if (data[0] == 0) {
            state->done = 1;
        } else if (p->tot_len >= 28 && (data[0] & 0xF0) == 0x40) {
            u16_t ip_len = (data[0] & 0x0F) * 4;
            if (p->tot_len >= ip_len + 8 && data[ip_len] == 0) {
                state->done = 1;
            }
        }
    }
    return 0;
}

int network_icmp_single_ping(ipv4_address_t *dest) {
    if (!lwip_initialized) return -2;

    ping_state_t state = { .done = 0, .start = 0 };

    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
    raw_recv(pcb, ping_recv, &state);
    raw_bind(pcb, IP_ADDR_ANY);

    ip_addr_t dest_addr;
    IP_SET_TYPE_VAL(dest_addr, IPADDR_TYPE_V4);
    IP4_ADDR(ip_2_ip4(&dest_addr), dest->bytes[0], dest->bytes[1], dest->bytes[2], dest->bytes[3]);

    struct pbuf *p = pbuf_alloc(PBUF_IP, 8 + 56, PBUF_RAM);
    if (!p) { raw_remove(pcb); spinlock_release_irqrestore(&network_lock, flags); return -1; }
    u8_t *data = (u8_t *)p->payload;
    data[0] = 8; data[1] = 0; data[2] = 0; data[3] = 0; // ICMP Type 8 (Echo Request), Code 0
    data[4] = 0; data[5] = 1; // ID
    data[6] = (u8_t)(ping_seq >> 8); data[7] = (u8_t)(ping_seq & 0xFF);
    ping_seq++;
    for (int j = 0; j < 56; j++) data[8+j] = (u8_t)('a' + (j % 26));

    extern u16_t inet_chksum(const void *dataptr, u16_t len);
    u16_t chk = inet_chksum(data, 8 + 56);
    *(u16_t *)&data[2] = chk;

    state.start = sys_now();
    raw_sendto(pcb, p, &dest_addr);
    pbuf_free(p);
    spinlock_release_irqrestore(&network_lock, flags);

    while (1) {
        network_process_frames();
        flags = spinlock_acquire_irqsave(&network_lock);
        int got = state.done;
        spinlock_release_irqrestore(&network_lock, flags);
        if (got) break;
        if ((sys_now() - state.start) >= 2000) break;
        k_delay(5);
    }

    flags = spinlock_acquire_irqsave(&network_lock);
    raw_remove(pcb);
    spinlock_release_irqrestore(&network_lock, flags);

    if (!state.done) return -1;
    return (int)(sys_now() - state.start);
}

int network_get_frames_received(void) { return (int)lwip_stats.link.recv; }
int network_get_udp_packets_received(void) { return (int)lwip_stats.udp.recv; }
int network_get_frames_sent(void) { return (int)lwip_stats.link.xmit; }
int network_get_udp_callbacks_called(void) { return 0; }
int network_get_e1000_receive_calls(void) { return 0; }
int network_get_e1000_receive_empty(void) { return 0; }
int network_get_process_calls(void) { return (int)lwip_stats.link.drop; }

static err_t tcp_socket_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)err; (void)tpcb;
    process_fd_socket_t *sock = (process_fd_socket_t *)arg;
    if (!sock) {
        if (p) pbuf_free(p);
        return ERR_VAL;
    }

    if (p == NULL) {
        sock->tcp_closed = 1;
        wait_queue_wake_all(&sock->rx_sb.waitq);
        wait_queue_wake_all(&sock->rx_waitq);
        return ERR_OK;
    }

    sockbuf_append(&sock->rx_sb, p, NULL, 0);
    pbuf_free(p);
    wait_queue_wake_all(&sock->rx_sb.waitq);
    wait_queue_wake_all(&sock->rx_waitq);
    return ERR_OK;
}

static void tcp_socket_err_callback(void *arg, err_t err) {
    (void)err;
    process_fd_socket_t *sock = (process_fd_socket_t *)arg;
    if (sock) {
        sock->pcb = NULL;
        sock->tcp_connect_error = 1;
        wait_queue_wake_all(&sock->rx_sb.waitq);
        wait_queue_wake_all(&sock->rx_waitq);
    }
}

static err_t tcp_socket_connected_callback(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;
    process_fd_socket_t *sock = (process_fd_socket_t *)arg;
    if (sock) {
        if (err == ERR_OK) {
            sock->tcp_connect_done = 1;
        } else {
            sock->tcp_connect_error = 1;
        }
        wait_queue_wake_all(&sock->rx_sb.waitq);
        wait_queue_wake_all(&sock->rx_waitq);
    }
    return ERR_OK;
}

static err_t tcp_socket_accept_callback(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)err;
    process_fd_socket_t *listener = (process_fd_socket_t *)arg;
    if (!listener) return ERR_VAL;

    process_fd_socket_t *client = process_socket_create();
    if (!client) {
        return ERR_MEM;
    }

    client->domain = 2; // AF_INET
    client->pcb = new_pcb;
    client->is_connected = 1;
    client->is_bound = 1;
    client->tcp_closed = 0;
    client->tcp_connect_error = 0;
    client->tcp_connect_done = 1;

    tcp_arg(new_pcb, client);
    tcp_recv(new_pcb, tcp_socket_recv_callback);
    tcp_err(new_pcb, tcp_socket_err_callback);

    uint64_t lflags = spinlock_acquire_irqsave(&listener->lock);
    int max_backlog = (listener->backlog_max > 0 && listener->backlog_max <= 128) ? listener->backlog_max : 128;
    if (listener->accept_queue_count < max_backlog) {
        accept_queue_entry_t *entry = (accept_queue_entry_t *)kmalloc(sizeof(accept_queue_entry_t));
        if (!entry) {
            spinlock_release_irqrestore(&listener->lock, lflags);
            process_socket_release(client);
            return ERR_MEM;
        }
        entry->client_sock = client;
        entry->next = NULL;
        if (!listener->accept_tail) {
            listener->accept_head = entry;
            listener->accept_tail = entry;
        } else {
            listener->accept_tail->next = entry;
            listener->accept_tail = entry;
        }
        listener->accept_queue_count++;
        spinlock_release_irqrestore(&listener->lock, lflags);
        wait_queue_wake_all(&listener->accept_waitq);
        return ERR_OK;
    } else {
        spinlock_release_irqrestore(&listener->lock, lflags);
        process_socket_release(client);
        return ERR_MEM;
    }
}

int network_socket_bind(void *s, uint32_t ip_val, uint16_t port) {
    extern void serial_write(const char *str);
    extern void serial_write_num(uint64_t n);
    serial_write("[network] bind: entered\n");

    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);

    if (sock->type == 2) {
        if (!sock->pcb) {
            sock->pcb = udp_new();
            if (!sock->pcb) {
                spinlock_release_irqrestore(&network_lock, flags);
                return -1;
            }
            ip_set_option((struct udp_pcb *)sock->pcb, SOF_BROADCAST);
            udp_recv((struct udp_pcb *)sock->pcb, udp_socket_recv_callback, sock);
        }
        ip_addr_t bind_ip;
        IP_SET_TYPE_VAL(bind_ip, IPADDR_TYPE_V4);
        ip_2_ip4(&bind_ip)->addr = ip_val;
        err_t err = udp_bind((struct udp_pcb *)sock->pcb, &bind_ip, port);
        spinlock_release_irqrestore(&network_lock, flags);
        return err == ERR_OK ? 0 : -1;
    }

    serial_write("[network] bind: sock->pcb is ");
    if (sock->pcb) {
        serial_write("not NULL\n");
    } else {
        serial_write("NULL, calling tcp_new...\n");
        sock->pcb = tcp_new();
        if (!sock->pcb) {
            serial_write("[network] bind: tcp_new returned NULL!\n");
            spinlock_release_irqrestore(&network_lock, flags);
            return -1;
        }
        serial_write("[network] bind: tcp_new succeeded\n");
        tcp_arg((struct tcp_pcb *)sock->pcb, sock);
    }

    ip_addr_t bind_ip;
    IP_SET_TYPE_VAL(bind_ip, IPADDR_TYPE_V4);
    ip_2_ip4(&bind_ip)->addr = ip_val;

    serial_write("[network] bind: calling tcp_bind...\n");
    err_t err = tcp_bind((struct tcp_pcb *)sock->pcb, &bind_ip, port);
    serial_write("[network] bind: tcp_bind returned ");
    if (err < 0) {
        serial_write("-");
        serial_write_num(-err);
    } else {
        serial_write_num(err);
    }
    serial_write("\n");

    if (err != ERR_OK) {
        serial_write("[network] tcp_bind failed\n");
    }
    spinlock_release_irqrestore(&network_lock, flags);
    return (int)err;
}

int network_socket_listen(void *s, int backlog) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    if (!sock->pcb) {
        spinlock_release_irqrestore(&network_lock, flags);
        return -1;
    }

    if (backlog > 0) {
        sock->backlog_max = backlog > 128 ? 128 : (uint32_t)backlog;
    }

    struct tcp_pcb *l_pcb = tcp_listen((struct tcp_pcb *)sock->pcb);
    if (!l_pcb) {
        spinlock_release_irqrestore(&network_lock, flags);
        return -1;
    }
    sock->pcb = l_pcb;
    tcp_arg((struct tcp_pcb *)sock->pcb, sock);
    tcp_accept((struct tcp_pcb *)sock->pcb, tcp_socket_accept_callback);
    spinlock_release_irqrestore(&network_lock, flags);
    return 0;
}

int network_socket_connect(void *s, uint32_t ip_val, uint16_t port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);

    if (sock->type == 2) {
        if (!sock->pcb) {
            sock->pcb = udp_new();
            if (!sock->pcb) {
                spinlock_release_irqrestore(&network_lock, flags);
                return -1;
            }
            udp_recv((struct udp_pcb *)sock->pcb, udp_socket_recv_callback, sock);
        }
        ip_addr_t conn_ip;
        IP_SET_TYPE_VAL(conn_ip, IPADDR_TYPE_V4);
        ip_2_ip4(&conn_ip)->addr = ip_val;
        err_t err = udp_connect((struct udp_pcb *)sock->pcb, &conn_ip, port);
        spinlock_release_irqrestore(&network_lock, flags);
        return err == ERR_OK ? 0 : -1;
    }

    if (sock->pcb) {
        tcp_abort((struct tcp_pcb *)sock->pcb);
        sock->pcb = NULL;
    }

    sock->pcb = tcp_new();
    if (!sock->pcb) {
        spinlock_release_irqrestore(&network_lock, flags);
        return -1;
    }

    sock->tcp_connect_done = 0;
    sock->tcp_connect_error = 0;
    sock->tcp_closed = 0;

    tcp_arg((struct tcp_pcb *)sock->pcb, sock);
    tcp_recv((struct tcp_pcb *)sock->pcb, tcp_socket_recv_callback);
    tcp_err((struct tcp_pcb *)sock->pcb, tcp_socket_err_callback);

    ip_addr_t dest_addr;
    IP_SET_TYPE_VAL(dest_addr, IPADDR_TYPE_V4);
    ip_2_ip4(&dest_addr)->addr = ip_val;

    err_t err = tcp_connect((struct tcp_pcb *)sock->pcb, &dest_addr, port, tcp_socket_connected_callback);
    spinlock_release_irqrestore(&network_lock, flags);

    if (err != ERR_OK) return -1;

    while (!sock->tcp_connect_done && !sock->tcp_connect_error) {
        wait_queue_wait(&sock->rx_sb.waitq);
    }
    return sock->tcp_connect_done ? 0 : -1;
}

static u8_t raw_icmp_recv_callback(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)pcb;
    process_fd_socket_t *sock = (process_fd_socket_t *)arg;
    if (!sock || !p) return 0;

    sockbuf_append(&sock->rx_sb, p, addr, 0);
    return 1;
}

int network_socket_recvfrom(void *s, void *buf, size_t max_len, int nonblock, uint32_t *from_ip, uint16_t *from_port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;

    uint32_t timeout_ms = sock->rcvtimeo;
    uint32_t start_time = sys_now();

    while (sockbuf_is_empty(&sock->rx_sb)) {
        if (nonblock) {
            return -2; // EWOULDBLOCK
        }
        if (timeout_ms > 0 && (sys_now() - start_time >= timeout_ms)) {
            return -2; // EWOULDBLOCK / Timeout
        }
        if (sock->tcp_closed) return 0;
        if (timeout_ms > 0) {
            wait_queue_wait_timeout(&sock->rx_sb.waitq, timeout_ms);
        } else {
            wait_queue_wait(&sock->rx_sb.waitq);
        }
    }

    ip_addr_t src_ip;
    uint16_t src_port = 0;
    int copied = sockbuf_read(&sock->rx_sb, buf, max_len, &src_ip, &src_port, 0);

    if (from_ip) *from_ip = ip_2_ip4(&src_ip)->addr;
    if (from_port) *from_port = src_port;

    return copied;
}

int network_socket_sendto(void *s, const void *data, size_t len, uint32_t dest_ip, uint16_t dest_port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&network_lock);

    if (sock->protocol == 1 || sock->type == 3) { // IPPROTO_ICMP or SOCK_RAW
        if (!sock->pcb) {
            sock->pcb = raw_new(IP_PROTO_ICMP);
            if (!sock->pcb) {
                spinlock_release_irqrestore(&network_lock, flags);
                return -1;
            }
            raw_recv((struct raw_pcb *)sock->pcb, raw_icmp_recv_callback, sock);
            raw_bind((struct raw_pcb *)sock->pcb, IP4_ADDR_ANY);
        }

        struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)len, PBUF_RAM);
        if (!p) {
            spinlock_release_irqrestore(&network_lock, flags);
            return -1;
        }
        memcpy(p->payload, data, len);

        ip_addr_t dst_addr;
        IP_SET_TYPE_VAL(dst_addr, IPADDR_TYPE_V4);
        ip_2_ip4(&dst_addr)->addr = dest_ip;

        err_t err = raw_sendto((struct raw_pcb *)sock->pcb, p, &dst_addr);
        pbuf_free(p);
        spinlock_release_irqrestore(&network_lock, flags);

        return err == ERR_OK ? (int)len : -1;
    }

    if (!sock->pcb) {
        sock->pcb = udp_new();
        if (!sock->pcb) {
            spinlock_release_irqrestore(&network_lock, flags);
            return -1;
        }
        udp_recv((struct udp_pcb *)sock->pcb, udp_socket_recv_callback, sock);
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) {
        spinlock_release_irqrestore(&network_lock, flags);
        return -1;
    }
    memcpy(p->payload, data, len);

    ip_addr_t dst_addr;
    IP_SET_TYPE_VAL(dst_addr, IPADDR_TYPE_V4);
    ip_2_ip4(&dst_addr)->addr = dest_ip;

    if (dest_ip == 0xFFFFFFFF || dest_ip == 0) {
        ip_set_option((struct udp_pcb *)sock->pcb, SOF_BROADCAST);
    }

    extern struct netif *netif_default;
    err_t err;
    if ((dest_ip == 0xFFFFFFFF || dest_ip == 0) && netif_default) {
        err = udp_sendto_if((struct udp_pcb *)sock->pcb, p, &dst_addr, dest_port, netif_default);
    } else {
        err = udp_sendto((struct udp_pcb *)sock->pcb, p, &dst_addr, dest_port);
    }
    pbuf_free(p);
    spinlock_release_irqrestore(&network_lock, flags);

    return err == ERR_OK ? (int)len : -1;
}

int network_socket_recv(void *s, void *buf, size_t max_len, int nonblock) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!lwip_initialized || !sock) return -1;
    if (sock->type == 2) {
        return network_socket_recvfrom(s, buf, max_len, nonblock, NULL, NULL);
    }

    while (sockbuf_is_empty(&sock->rx_sb)) {
        if (sock->tcp_closed) return 0;
        if (sock->tcp_connect_error) return -1;
        if (nonblock) return -2;

        network_process_frames();
        if (!sockbuf_is_empty(&sock->rx_sb)) break;

        wait_queue_wait_timeout(&sock->rx_sb.waitq, 5);
    }

    int copied = sockbuf_read(&sock->rx_sb, buf, max_len, NULL, NULL, 0);
    if (copied > 0 && sock->pcb) {
        uint64_t flags = spinlock_acquire_irqsave(&network_lock);
        if (sock->pcb) {
            tcp_recved((struct tcp_pcb *)sock->pcb, (u16_t)copied);
            tcp_output((struct tcp_pcb *)sock->pcb);
        }
        spinlock_release_irqrestore(&network_lock, flags);
    }
    return copied;
}

int network_socket_send(void *s, const void *data, size_t len, int nonblock) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;
    if (sock->type == 2) {
        if (!sock->pcb) return -1;
        struct udp_pcb *upcb = (struct udp_pcb *)sock->pcb;
        if (ip_addr_isany(&upcb->remote_ip)) return -1;
        return network_socket_sendto(s, data, len, ip_2_ip4(&upcb->remote_ip)->addr, upcb->remote_port);
    }

    while (1) {
        uint64_t flags = spinlock_acquire_irqsave(&network_lock);
        if (!sock->pcb || sock->tcp_closed || sock->tcp_connect_error) {
            spinlock_release_irqrestore(&network_lock, flags);
            return -1;
        }

        struct tcp_pcb *tpcb = (struct tcp_pcb *)sock->pcb;
        u16_t snd_buf = tcp_sndbuf(tpcb);
        if (snd_buf >= len) {
            err_t err = tcp_write(tpcb, data, len, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) {
                spinlock_release_irqrestore(&network_lock, flags);
                return -1;
            }
            tcp_output(tpcb);
            spinlock_release_irqrestore(&network_lock, flags);
            return (int)len;
        } else if (snd_buf > 0) {
            err_t err = tcp_write(tpcb, data, snd_buf, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) {
                spinlock_release_irqrestore(&network_lock, flags);
                return -1;
            }
            tcp_output(tpcb);
            spinlock_release_irqrestore(&network_lock, flags);
            return (int)snd_buf;
        }
        spinlock_release_irqrestore(&network_lock, flags);

        if (nonblock) return -2; // EWOULDBLOCK
        wait_queue_wait_timeout(&sock->rx_waitq, 5);
    }
}

#define MAX_RAW_TAPS 16
static process_fd_socket_t *raw_taps[MAX_RAW_TAPS];
static spinlock_t raw_taps_lock = SPINLOCK_INIT;

void raw_tap_register(process_fd_socket_t *sock) {
    if (!sock) return;
    uint64_t flags = spinlock_acquire_irqsave(&raw_taps_lock);
    for (int i = 0; i < MAX_RAW_TAPS; i++) {
        if (raw_taps[i] == sock) { spinlock_release_irqrestore(&raw_taps_lock, flags); return; }
    }
    for (int i = 0; i < MAX_RAW_TAPS; i++) {
        if (!raw_taps[i]) {
            raw_taps[i] = sock;
            break;
        }
    }
    spinlock_release_irqrestore(&raw_taps_lock, flags);
}

void raw_tap_unregister(process_fd_socket_t *sock) {
    if (!sock) return;
    uint64_t flags = spinlock_acquire_irqsave(&raw_taps_lock);
    for (int i = 0; i < MAX_RAW_TAPS; i++) {
        if (raw_taps[i] == sock) {
            raw_taps[i] = NULL;
            break;
        }
    }
    spinlock_release_irqrestore(&raw_taps_lock, flags);
}

void raw_tap_broadcast(const void *data, size_t len) {
    if (!data || len == 0) return;
    uint64_t flags = spinlock_acquire_irqsave(&raw_taps_lock);
    for (int i = 0; i < MAX_RAW_TAPS; i++) {
        process_fd_socket_t *sock = raw_taps[i];
        if (sock) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
            if (p) {
                pbuf_take(p, data, (u16_t)len);
                if (sockbuf_append(&sock->rx_sb, p, NULL, 0) < 0) {
                    pbuf_free(p);
                }
            }
        }
    }
    spinlock_release_irqrestore(&raw_taps_lock, flags);
}

void network_socket_close(void *s) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return;

    raw_tap_unregister(sock);

    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    if (sock->pcb) {
        if (sock->protocol == 1 || sock->type == 3) {
            raw_remove((struct raw_pcb *)sock->pcb);
        } else if (sock->type == 2) {
            udp_remove((struct udp_pcb *)sock->pcb);
        } else {
            tcp_arg((struct tcp_pcb *)sock->pcb, NULL);
            if (!sock->is_listening) {
                tcp_recv((struct tcp_pcb *)sock->pcb, NULL);
                tcp_err((struct tcp_pcb *)sock->pcb, NULL);
            } else {
                tcp_accept((struct tcp_pcb *)sock->pcb, NULL);
            }

            err_t err = tcp_close((struct tcp_pcb *)sock->pcb);
            if (err != ERR_OK) {
                tcp_abort((struct tcp_pcb *)sock->pcb);
            }
        }
        sock->pcb = NULL;
    }
    sockbuf_destroy(&sock->rx_sb);

    uint64_t lflags = spinlock_acquire_irqsave(&sock->lock);
    accept_queue_entry_t *curr = sock->accept_head;
    sock->accept_head = NULL;
    sock->accept_tail = NULL;
    sock->accept_queue_count = 0;
    spinlock_release_irqrestore(&sock->lock, lflags);

    while (curr) {
        accept_queue_entry_t *next = curr->next;
        if (curr->client_sock) {
            process_socket_release((process_fd_socket_t *)curr->client_sock);
        }
        kfree_null(curr);
        curr = next;
    }

    spinlock_release_irqrestore(&network_lock, flags);
}

void network_socket_get_remote_info(void *s, uint16_t *port, uint32_t *ip) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (sock && sock->pcb) {
        struct tcp_pcb *c_pcb = (struct tcp_pcb *)sock->pcb;
        if (port) *port = c_pcb->remote_port;
        if (ip) *ip = ip_2_ip4(&c_pcb->remote_ip)->addr;
    }
}

int network_setsockopt(void *s, int level, int optname, const void *optval, size_t optlen) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !optval) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&sock->lock);
    if (level == 1) { // SOL_SOCKET
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            uint32_t ms = 0;
            if (optlen >= 16) {
                const uint64_t *tv = (const uint64_t *)optval;
                ms = (uint32_t)(tv[0] * 1000 + tv[1] / 1000);
            } else if (optlen >= 8) {
                const uint32_t *tv = (const uint32_t *)optval;
                ms = tv[0] * 1000 + tv[1] / 1000;
            } else if (optlen >= 4) {
                ms = *(const uint32_t *)optval;
            }
            if (optname == SO_RCVTIMEO) sock->rcvtimeo = ms;
            else sock->sndtimeo = ms;
        } else if (optlen >= sizeof(int)) {
            int val = *(const int *)optval;
            switch (optname) {
                case SO_REUSEADDR:  sock->reuseaddr = val ? 1 : 0; break;
                case SO_REUSEPORT: sock->reuseport = val ? 1 : 0; break;
                case SO_KEEPALIVE:
                    sock->keepalive = val ? 1 : 0;
                    if (sock->pcb && sock->type == 1) {
                        uint64_t nflags = spinlock_acquire_irqsave(&network_lock);
                        if (val) ip_set_option((struct tcp_pcb *)sock->pcb, SOF_KEEPALIVE);
                        else ip_reset_option((struct tcp_pcb *)sock->pcb, SOF_KEEPALIVE);
                        spinlock_release_irqrestore(&network_lock, nflags);
                    }
                    break;
                case SO_RCVBUF: sock->rx_sb.sb_hiwat = (uint32_t)val; break;
                case SO_SNDBUF: sock->tx_sb.sb_hiwat = (uint32_t)val; break;
                default: break;
            }
        }
    } else if (level == 6) { // IPPROTO_TCP
        if (optlen >= sizeof(int)) {
            int val = *(const int *)optval;
            if (optname == TCP_NODELAY) {
                sock->nodelay = val ? 1 : 0;
                if (sock->pcb) {
                    uint64_t nflags = spinlock_acquire_irqsave(&network_lock);
                    if (val) tcp_nagle_disable((struct tcp_pcb *)sock->pcb);
                    else tcp_nagle_enable((struct tcp_pcb *)sock->pcb);
                    spinlock_release_irqrestore(&network_lock, nflags);
                }
            }
        }
    }
    spinlock_release_irqrestore(&sock->lock, flags);
    return 0;
}

int network_getsockopt(void *s, int level, int optname, void *optval, size_t *optlen) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !optval || !optlen) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&sock->lock);
    if (level == 1) { // SOL_SOCKET
        int *val = (int *)optval;
        switch (optname) {
            case SO_REUSEADDR:  *val = sock->reuseaddr; break;
            case SO_REUSEPORT: *val = sock->reuseport; break;
            case SO_KEEPALIVE: *val = sock->keepalive; break;
            case SO_RCVBUF:    *val = (int)sock->rx_sb.sb_hiwat; break;
            case SO_SNDBUF:    *val = (int)sock->tx_sb.sb_hiwat; break;
            case SO_RCVTIMEO:  *val = (int)sock->rcvtimeo; break;
            case SO_SNDTIMEO:  *val = (int)sock->sndtimeo; break;
            default: *val = 0; break;
        }
        *optlen = sizeof(int);
    }
    spinlock_release_irqrestore(&sock->lock, flags);
    return 0;
}

int network_if_ioctl(unsigned long cmd, void *arg) {
    if (!lwip_initialized || !arg) return -1;

    char *ifname = (char *)arg;
    struct netif *target_netif = &nic_netif;

    if (ifname[0] != '\0') {
        if (strncmp(ifname, "lo", 2) == 0) {
            struct netif *found = netif_find("lo");
            if (found) {
                target_netif = found;
            }
        } else {
            struct netif *found = netif_find(ifname);
            if (found) {
                target_netif = found;
            }
        }
    }

    switch (cmd) {
        case 0x8912: { // SIOCGIFCONF
            extern const char* nic_get_active_name(void);
            const char* nic_name = nic_get_active_name();
            if (!nic_name) nic_name = "eth0";

            struct {
                int ifc_len;
                char pad[4];
                char *ifc_buf;
            } *ifc = (void *)arg;

            struct ifreq_entry {
                char ifr_name[16];
                uint16_t sa_family;
                uint16_t sa_port;
                uint32_t sin_addr;
                char zero[8];
            };

            int count = 0;
            struct netif *n;
            NETIF_FOREACH(n) {
                count++;
            }

            int req_size = sizeof(struct ifreq_entry) * (count > 0 ? count : 1);
            if (!ifc->ifc_buf || ifc->ifc_len < req_size) {
                ifc->ifc_len = req_size;
                return 0;
            }

            struct ifreq_entry *req = (struct ifreq_entry *)ifc->ifc_buf;
            memset(req, 0, req_size);

            int idx = 0;
            NETIF_FOREACH(n) {
                if (n->name[0] == 'l' && n->name[1] == 'o') {
                    strncpy(req[idx].ifr_name, "lo", 15);
                } else {
                    strncpy(req[idx].ifr_name, nic_name, 15);
                }
                req[idx].sa_family = 2; // AF_INET
                req[idx].sin_addr = ip4_addr_get_u32(netif_ip4_addr(n));
                idx++;
            }

            ifc->ifc_len = req_size;
            return 0;
        }
        case 0x8915:
        case 0xc0206921: { // SIOCGIFADDR
            uint16_t *family = (uint16_t *)((char *)arg + 16);
            uint32_t *ip_ptr = (uint32_t *)((char *)arg + 20);
            *family = 2; // AF_INET
            *ip_ptr = ip4_addr_get_u32(netif_ip4_addr(target_netif));
            return 0;
        }
        case 0x8916: { // SIOCSIFADDR
            uint32_t ip_val = *(uint32_t *)((char *)arg + 20);
            ip4_addr_t ipaddr;
            ip4_addr_set_u32(&ipaddr, ip_val);
            netif_set_ipaddr(target_netif, &ipaddr);
            netif_set_up(target_netif);
            netif_set_link_up(target_netif);
            if (target_netif == &nic_netif) {
                netif_set_default(target_netif);
            }
            return 0;
        }
        case 0x891b: { // SIOCGIFNETMASK
            uint16_t *family = (uint16_t *)((char *)arg + 16);
            uint32_t *ip_ptr = (uint32_t *)((char *)arg + 20);
            *family = 2;
            *ip_ptr = ip4_addr_get_u32(netif_ip4_netmask(target_netif));
            return 0;
        }
        case 0x891c: { // SIOCSIFNETMASK
            uint32_t mask_val = *(uint32_t *)((char *)arg + 20);
            ip4_addr_t netmask;
            ip4_addr_set_u32(&netmask, mask_val);
            netif_set_netmask(target_netif, &netmask);
            return 0;
        }
        case 0x8913: { // SIOCGIFFLAGS
            short *flags = (short *)((char *)arg + 16);
            if (target_netif->name[0] == 'l' && target_netif->name[1] == 'o') {
                *flags = 0x1 | 0x8 | 0x40; // IFF_UP | IFF_LOOPBACK | IFF_RUNNING
            } else {
                *flags = 0x1 | 0x2 | 0x40; // IFF_UP | IFF_BROADCAST | IFF_RUNNING
            }
            return 0;
        }
        case 0x8914: { // SIOCSIFFLAGS
            return 0;
        }
        case 0x890B: { // SIOCADDRT (Add default gateway)
            uint32_t gw_val = *(uint32_t *)((char *)arg + 20);
            ip4_addr_t gw_addr;
            ip4_addr_set_u32(&gw_addr, gw_val);
            netif_set_gw(target_netif, &gw_addr);
            return 0;
        }
        case 0x8927: { // SIOCGIFHWADDR
            uint16_t *family = (uint16_t *)((char *)arg + 16);
            uint8_t *mac_ptr = (uint8_t *)((char *)arg + 18);
            *family = 1; // ARPHRD_ETHER
            if (target_netif->name[0] == 'l' && target_netif->name[1] == 'o') {
                memset(mac_ptr, 0, 6);
            } else {
                nic_get_mac_address(mac_ptr);
            }
            return 0;
        }
        default:
            return -1;
    }
}

int network_socket_bind_v6(void *s, const ipv6_address_t *ip6, uint16_t port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !ip6) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);

    ip_addr_t bind_ip;
    IP_SET_TYPE_VAL(bind_ip, IPADDR_TYPE_V6);
    memcpy(ip_2_ip6(&bind_ip)->addr, ip6->bytes, 16);

    if (sock->type == 2) {
        if (!sock->pcb) {
            sock->pcb = udp_new();
            if (!sock->pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
            udp_recv((struct udp_pcb *)sock->pcb, udp_socket_recv_callback, sock);
        }
        err_t err = udp_bind((struct udp_pcb *)sock->pcb, &bind_ip, port);
        spinlock_release_irqrestore(&network_lock, flags);
        return err == ERR_OK ? 0 : -1;
    }

    if (!sock->pcb) {
        sock->pcb = tcp_new();
        if (!sock->pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
        tcp_arg((struct tcp_pcb *)sock->pcb, sock);
    }
    err_t err = tcp_bind((struct tcp_pcb *)sock->pcb, &bind_ip, port);
    spinlock_release_irqrestore(&network_lock, flags);
    return err == ERR_OK ? 0 : -1;
}

int network_socket_connect_v6(void *s, const ipv6_address_t *ip6, uint16_t port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !ip6) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&network_lock);

    ip_addr_t dest_ip;
    IP_SET_TYPE_VAL(dest_ip, IPADDR_TYPE_V6);
    memcpy(ip_2_ip6(&dest_ip)->addr, ip6->bytes, 16);

    if (sock->type == 2) {
        if (!sock->pcb) {
            sock->pcb = udp_new();
            if (!sock->pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
            udp_recv((struct udp_pcb *)sock->pcb, udp_socket_recv_callback, sock);
        }
        err_t err = udp_connect((struct udp_pcb *)sock->pcb, &dest_ip, port);
        spinlock_release_irqrestore(&network_lock, flags);
        return err == ERR_OK ? 0 : -1;
    }

    if (sock->pcb) { tcp_abort((struct tcp_pcb *)sock->pcb); sock->pcb = NULL; }
    sock->pcb = tcp_new();
    if (!sock->pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }

    sock->tcp_connect_done = 0;
    sock->tcp_connect_error = 0;
    sock->tcp_closed = 0;
    tcp_arg((struct tcp_pcb *)sock->pcb, sock);
    tcp_recv((struct tcp_pcb *)sock->pcb, tcp_socket_recv_callback);
    tcp_err((struct tcp_pcb *)sock->pcb, tcp_socket_err_callback);

    err_t err = tcp_connect((struct tcp_pcb *)sock->pcb, &dest_ip, port, tcp_socket_connected_callback);
    spinlock_release_irqrestore(&network_lock, flags);

    if (err != ERR_OK) return -1;

    uint32_t start = sys_now();
    while (sys_now() - start < 15000) {
        flags = spinlock_acquire_irqsave(&network_lock);
        if (sock->tcp_connect_done) { spinlock_release_irqrestore(&network_lock, flags); return 0; }
        if (sock->tcp_connect_error) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
        spinlock_release_irqrestore(&network_lock, flags);
        k_sleep(1);
    }
    return -1;
}

int network_socket_recvfrom_v6(void *s, void *buf, size_t max_len, int nonblock, ipv6_address_t *from_ip, uint16_t *from_port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;

    ip_addr_t src_ip;
    uint16_t src_port = 0;

    while (sockbuf_is_empty(&sock->rx_sb)) {
        if (nonblock) return -2;
        if (sock->tcp_closed) return 0;
        wait_queue_wait(&sock->rx_sb.waitq);
    }

    int copied = sockbuf_read(&sock->rx_sb, buf, max_len, &src_ip, &src_port, 0);

    if (from_ip) memcpy(from_ip->bytes, ip_2_ip6(&src_ip)->addr, 16);
    if (from_port) *from_port = src_port;

    return copied;
}

int network_socket_sendto_v6(void *s, const void *data, size_t len, const ipv6_address_t *dest_ip, uint16_t dest_port) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !dest_ip) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    if (!sock->pcb) {
        sock->pcb = udp_new();
        if (!sock->pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
        udp_recv((struct udp_pcb *)sock->pcb, udp_socket_recv_callback, sock);
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
    memcpy(p->payload, data, len);

    ip_addr_t dst_addr;
    IP_SET_TYPE_VAL(dst_addr, IPADDR_TYPE_V6);
    memcpy(ip_2_ip6(&dst_addr)->addr, dest_ip->bytes, 16);

    err_t err = udp_sendto((struct udp_pcb *)sock->pcb, p, &dst_addr, dest_port);
    pbuf_free(p);
    spinlock_release_irqrestore(&network_lock, flags);
    return err == ERR_OK ? (int)len : -1;
}

typedef struct {
    volatile int done;
    volatile uint32_t start;
} ping6_state_t;

static u8_t ping6_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)pcb; (void)addr;
    ping6_state_t *state = (ping6_state_t *)arg;
    if (state && !state->done && p && p->tot_len >= 8) {
        u8_t *data = (u8_t *)p->payload;
        if (data[0] == 129) { // ICMPv6 Echo Reply
            state->done = 1;
        }
    }
    return 0;
}

int network_icmp6_single_ping(const ipv6_address_t *dest) {
    if (!lwip_initialized || !dest) return -2;

    ping6_state_t state = { .done = 0, .start = 0 };

    uint64_t flags = spinlock_acquire_irqsave(&network_lock);
    struct raw_pcb *pcb = raw_new(58 /* IP_PROTO_ICMP6 */);
    if (!pcb) { spinlock_release_irqrestore(&network_lock, flags); return -1; }
    raw_recv(pcb, ping6_recv, &state);
    raw_bind(pcb, IP6_ADDR_ANY);

    ip_addr_t dest_addr;
    IP_SET_TYPE_VAL(dest_addr, IPADDR_TYPE_V6);
    memcpy(ip_2_ip6(&dest_addr)->addr, dest->bytes, 16);

    struct pbuf *p = pbuf_alloc(PBUF_IP, 8 + 56, PBUF_RAM);
    if (!p) { raw_remove(pcb); spinlock_release_irqrestore(&network_lock, flags); return -1; }
    u8_t *data = (u8_t *)p->payload;
    data[0] = 128; data[1] = 0; // ICMPv6 Echo Request
    data[2] = 0; data[3] = 0;
    data[4] = 0; data[5] = 1;
    data[6] = 0; data[7] = 1;
    for (int j = 0; j < 56; j++) data[8 + j] = (u8_t)('a' + (j % 26));

    state.start = sys_now();
    raw_sendto(pcb, p, &dest_addr);
    pbuf_free(p);
    spinlock_release_irqrestore(&network_lock, flags);

    while (1) {
        network_process_frames();
        flags = spinlock_acquire_irqsave(&network_lock);
        int got = state.done;
        spinlock_release_irqrestore(&network_lock, flags);
        if (got) break;
        if ((sys_now() - state.start) >= 2000) break;
        k_delay(5);
    }

    flags = spinlock_acquire_irqsave(&network_lock);
    raw_remove(pcb);
    spinlock_release_irqrestore(&network_lock, flags);

    if (!state.done) return -1;
    return (int)(sys_now() - state.start);
}
