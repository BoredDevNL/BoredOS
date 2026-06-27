#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                     0
#define LWIP_SOCKET                1
#define LWIP_NETCONN               1

#define LWIP_PROVIDE_ERRNO         1

#define LWIP_ARP                   1
#define LWIP_ETHERNET              1
#define LWIP_ICMP                  1
#define LWIP_RAW                   1
#define LWIP_UDP                   1
#define LWIP_TCP                   1

#define LWIP_DHCP                  1
#define LWIP_DNS                   1
#define LWIP_IGMP                  0
#define LWIP_COMPAT_SOCKETS        1
#define LWIP_POSIX_SOCKETS_IO_NAMES 1

#define LWIP_NETIF_HOSTNAME        1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK   1

#define TCP_MSS                    1460
#define TCP_WND                    (32 * TCP_MSS)
#define TCP_SND_BUF                (32 * TCP_MSS)
#define TCP_SND_QUEUELEN           (4 * (TCP_SND_BUF/TCP_MSS))

#define MEM_ALIGNMENT              8

#define LWIP_CHKSUM_ALGORITHM      3

#define LWIP_STATS                 1

// Memory management
#define MEMP_MEM_MALLOC            0
#define MEM_LIBC_MALLOC            0
#define MEM_SIZE                   (16 * 1024 * 1024)
#define PBUF_POOL_SIZE             256
#define MEMP_NUM_TCP_SEG           128
#define MEMP_NUM_PBUF              256
#define MEMP_NUM_TCP_PCB           16
#define PBUF_POOL_FREE_OOSEQ_QUEUE_CALL() pbuf_free_ooseq()
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0

#endif /* LWIPOPTS_H */
