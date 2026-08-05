# BoredOS Network Stack Documentation

BoredOS features a POSIX-compliant, full-featured networking stack capable of handling Ethernet, IPv4, IPv6, TCP, UDP, ICMP, ARP, DHCP, DNS, UNIX Domain Sockets, Virtual TUN/TAP Interfaces, and Raw Packet Taps (`AF_PACKET`).

The stack is built on top of an embedded **lwIP (Lightweight IP)** protocol engine integrated with custom kernel socket abstractions, VFS character devices, and hardware NIC drivers.

---

## 1. Kernel Execution & Concurrency Model

* **Thread Polling Loop (`net_worker_loop`)**: Rather than handling packet parsing entirely inside hardware interrupt service routines (ISRs), BoredOS spawns a dedicated kernel background thread (`net_worker_loop` in `net/network.c`). This thread polls ring buffers from active NICs (`nic_netif_poll()`) and executes lwIP timers (`sys_check_timeouts()`) for TCP retransmissions, ARP expirations, and DHCP leases.
* **Synchronization (`network_lock`)**: All lwIP core data structures and callbacks are synchronized using a coarse kernel spinlock (`network_lock`).
* **Socket Buffering (`sockbuf_t`)**: Each socket handle contains dedicated receive and transmit buffer queues (`sockbuf_t` in `net/sockbuf.c`) protected by spinlocks and integrated with kernel wait queues (`wait_queue_t`). Blocking socket calls sleep on wait queues until data or connection state changes occur.

---

## 2. Supported Protocol Domains & Socket Types

BoredOS supports three main socket domains:

| Domain | Constants | Supported Types | Key Features |
| :--- | :--- | :--- | :--- |
| **IPv4 / IPv6** | `AF_INET` (2), `AF_INET6` (10) | `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_RAW` | Full TCP state machine, UDP datagrams, ICMP echo, DNS resolution, non-blocking I/O. |
| **UNIX Local** | `AF_UNIX` (1) | `SOCK_STREAM`, `SOCK_DGRAM` | High-performance local IPC, `socketpair()`, `SCM_RIGHTS` file descriptor passing over `sendmsg()`/`recvmsg()`. |
| **Raw Packet Tap** | `AF_PACKET` (17) | `SOCK_RAW` (3) | Captures raw link-layer Ethernet frames (ingress and egress), bypasses lwIP TCP/IP framing, allows direct raw packet injection. |

---

## 3. Hardware NICs & Virtual TUN/TAP Driver

### Network Interface Naming Conventions

BoredOS dynamically assigns BSD-style network interface names based on the detected PCI hardware driver or virtual device:

| NIC Hardware / Driver | Interface Name | Description |
| :--- | :--- | :--- |
| **Intel E1000** | `em0` | Intel 82540EM / 82545EM PCI Gigabit Ethernet |
| **Realtek RTL8139** | `re0` | Realtek RTL8139 Fast Ethernet |
| **Realtek RTL8111** | `re1` | Realtek RTL8111/8168 PCI Express Gigabit Ethernet |
| **VirtIO Net** | `vtnet0` | QEMU / KVM VirtIO Paravirtualized Network Device |
| **Virtual TUN/TAP** | `tun0` | Virtual Tunnel Interface created via `/dev/net/tun` |

### Virtual TUN/TAP Driver (`/dev/net/tun`)

BoredOS provides a Virtual TUN/TAP character device driver exposed at `/dev/net/tun`, `/dev/tun`, and `/dev/tun0`.

* **TUN Mode (`IFF_TUN`)**: Operates at Layer 3 (IP packets). Written IP packets are passed directly into lwIP's IP layer (`ip_input()`).
* **TAP Mode (`IFF_TAP`)**: Operates at Layer 2 (Ethernet frames). Written frames are fed into `ethernet_input()`.
* **Configuration (`TUNSETIFF`)**: Issued via `ioctl(fd, TUNSETIFF, &ifr)`. Creates and registers a virtual `tun0` `netif` interface with lwIP.

---

## 4. How to Use Network Clients & Daemons

BoredOS includes CLI network utilities for network configuration and remote connectivity.

### Automatic Network Setup (`dhclient`)

`dhclient` automatically negotiates an IP address, netmask, and default gateway via DHCP, and writes DNS resolvers to `/etc/resolv.conf`.

* **Automatic NIC Detection**: Running `dhclient` without arguments auto-detects the active physical hardware NIC (`em0`, `re0`, `vtnet0`, or `re1`) via `/sys/net/nic`:
  ```bash
  dhclient
  ```
* **Explicit NIC Override**: Specify the target interface explicitly if multiple interfaces are present:
  ```bash
  dhclient em0      # Intel E1000
  dhclient vtnet0   # VirtIO Net
  dhclient re0      # Realtek 8139
  ```

### Manual Network Configuration (`ifconfig` & `route`)

* **Query All Interfaces**:
  ```bash
  ifconfig
  ```
* **Assign Static IP Address**:
  ```bash
  ifconfig em0 10.0.2.15
  ```
* **Configure Default Gateway**:
  ```bash
  route add default 10.0.2.2
  ```

### HTTP/HTTPS Client (`curl`)

`curl` supports HTTP and HTTPS (TLS 1.2/1.3 powered by BearSSL):

* **Fetch Web Page**:
  ```bash
  curl http://example.com
  ```
* **Fetch HTTPS Secure Endpoint**:
  ```bash
  curl https://boredos.dev
  ```

### Embedded Web Server (`httpd`)

Start the built-in HTTP server daemon to serve web pages from `/Library/AppData/org.boredos.httpd/`:

```bash
httpd
```

### Remote Shell & Testing (`telnet`, `ping`, `ping6`)

* **Ping Host**:
  ```bash
  ping 10.0.2.2
  ping6 fe80::1
  ```
* **Connect via Telnet**:
  ```bash
  telnet 10.0.2.2 23
  ```

---

## 6. Userland Developer Guide & Code Examples

Applications running on BoredOS interact with the network stack using standard C POSIX socket functions provided by `mlibc`.

### Example A: TCP IPv4 Client (`AF_INET`)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(80);
    inet_pton(AF_INET, "93.184.216.34", &serv_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    const char *req = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    send(sockfd, req, strlen(req), 0);

    char buf[512];
    int len;
    while ((len = recv(sockfd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[len] = '\0';
        printf("%s", buf);
    }

    close(sockfd);
    return 0;
}
```

---

### Example B: UNIX Socket File Descriptor Passing (`SCM_RIGHTS`)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

void pass_fd_over_unix_socket(int sock_fd, int fd_to_send) {
    char dummy_byte = 'F';
    struct iovec iov = { .iov_base = &dummy_byte, .iov_len = 1 };
    char ctrl_buf[sizeof(struct cmsghdr) + sizeof(int)];
    memset(ctrl_buf, 0, sizeof(ctrl_buf));

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf)
    };

    struct cmsghdr *cmsg = (struct cmsghdr *)ctrl_buf;
    cmsg->cmsg_len = sizeof(ctrl_buf);
    cmsg->cmsg_level = 1; // SOL_SOCKET
    cmsg->cmsg_type = 1;  // SCM_RIGHTS
    *(int *)((char *)cmsg + sizeof(struct cmsghdr)) = fd_to_send;

    sendmsg(sock_fd, &msg, 0);
}

int receive_fd_over_unix_socket(int sock_fd) {
    char dummy_byte;
    struct iovec iov = { .iov_base = &dummy_byte, .iov_len = 1 };
    char ctrl_buf[sizeof(struct cmsghdr) + sizeof(int)];
    memset(ctrl_buf, 0, sizeof(ctrl_buf));

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf)
    };

    if (recvmsg(sock_fd, &msg, 0) < 0) return -1;

    struct cmsghdr *cmsg = (struct cmsghdr *)ctrl_buf;
    if (cmsg->cmsg_len >= sizeof(struct cmsghdr) + sizeof(int)) {
        return *(int *)((char *)cmsg + sizeof(struct cmsghdr));
    }
    return -1;
}
```

---

### Example C: Setting up a Virtual TUN Interface (`/dev/net/tun`)

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define TUNSETIFF 0x400454ca
#define IFF_TUN   0x0001

struct ifreq_tun {
    char ifr_name[16];
    short ifr_flags;
};

int create_tun_interface(void) {
    int tun_fd = open("/dev/net/tun", O_RDWR);
    if (tun_fd < 0) return -1;

    struct ifreq_tun ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN;

    if (ioctl(tun_fd, TUNSETIFF, &ifr) < 0) {
        close(tun_fd);
        return -1;
    }

    printf("Created TUN interface: %s (fd=%d)\n", ifr.ifr_name, tun_fd);
    return tun_fd; // Read/write raw IP packets to tun_fd
}
```

---

### Example D: Link-Layer Packet Sniffer (`AF_PACKET` / `SOCK_RAW`)

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void) {
    int raw_fd = socket(17 /* AF_PACKET */, 3 /* SOCK_RAW */, 0);
    if (raw_fd < 0) {
        perror("socket AF_PACKET");
        return 1;
    }

    printf("Listening for raw Ethernet frames...\n");
    char frame[2048];
    while (1) {
        int len = recv(raw_fd, frame, sizeof(frame), 0);
        if (len > 0) {
            unsigned char *mac = (unsigned char *)frame;
            printf("Captured %d bytes | Dest MAC: %02x:%02x:%02x:%02x:%02x:%02x | Src MAC: %02x:%02x:%02x:%02x:%02x:%02x | EtherType: 0x%02x%02x\n",
                len,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                mac[6], mac[7], mac[8], mac[9], mac[10], mac[11],
                mac[12], mac[13]);
        }
    }

    close(raw_fd);
    return 0;
}
```
