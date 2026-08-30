#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                     0
#define LWIP_SOCKET                1
#define LWIP_NETCONN               1

#define LWIP_PROVIDE_ERRNO         1

#ifdef BOREDOS_SYS_TIMEVAL
#define LWIP_TIMEVAL_PRIVATE       0
#else
#define LWIP_TIMEVAL_PRIVATE       1
#endif

#define LWIP_ARP                   1
#define LWIP_ETHERNET              1
#define LWIP_ICMP                  1
#define LWIP_RAW                   1
#define LWIP_UDP                   1
#define LWIP_TCP                   1

#define LWIP_DHCP                  0
#define LWIP_IP_ACCEPT_UDP_PORT(port) ((port) == PP_NTOHS(68))
#define LWIP_DNS                   0
#define LWIP_IGMP                  0
#define LWIP_COMPAT_SOCKETS        1
#define LWIP_POSIX_SOCKETS_IO_NAMES 1

#define LWIP_IPV6                  1
#define LWIP_IPV6_DHCP6            0
#define LWIP_IPV6_AUTOCONFIG       1
#define LWIP_IPV6_ICMP6            1
#define LWIP_IPV6_MLD              0
#define IPV6_FRAG_COPYHEADER       1

#define LWIP_SO_RCVTIMEO           1
#define LWIP_SO_SNDTIMEO           1
#define LWIP_SO_RCVBUF             1
#define LWIP_SO_REUSE              1

#define LWIP_NETIF_HOSTNAME        1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK   1

#define TCP_MSS                    1460
#define TCP_WND                    (512 * TCP_MSS)
#define TCP_SND_BUF                (512 * TCP_MSS)
#define TCP_SND_QUEUELEN           (4 * (TCP_SND_BUF/TCP_MSS))

#define LWIP_WND_SCALE             1
#define TCP_RCV_SCALE              5

#define TCP_SNDLOWAT               (2 * TCP_MSS)
#define LWIP_TCP_SACK_OUT          1
#define LWIP_TCP_TIMESTAMPS        1

#define MEM_ALIGNMENT              8

#define LWIP_CHKSUM_ALGORITHM      3

#define LWIP_STATS                 1

// Memory management
#define MEMP_MEM_MALLOC            0
#define MEM_LIBC_MALLOC            0
#define MEM_SIZE                   (16 * 1024 * 1024)
#define PBUF_POOL_SIZE             4096
#define MEMP_NUM_TCP_SEG           2048
#define MEMP_NUM_PBUF              4096
#define MEMP_NUM_TCP_PCB           256

#define MEMP_NUM_RAW_PCB           16
#define PBUF_POOL_FREE_OOSEQ_QUEUE_CALL() pbuf_free_ooseq()
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_HAVE_LOOPIF    1
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0

#endif /* LWIPOPTS_H */
