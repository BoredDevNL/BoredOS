#ifndef TUN_H
#define TUN_H

#include <stdint.h>
#include <stddef.h>
#include "lwip/netif.h"
#include "wait_queue.h"
#include "spinlock.h"
#include "sockbuf.h"

#define TUNSETIFF 0x400454ca
#define IFF_TUN   0x0001
#define IFF_TAP   0x0002
#define IFF_NO_PI 0x1000

typedef struct tun_device {
    char name[16];
    uint16_t flags;
    int is_active;
    struct netif netif;
    sockbuf_t tx_sb;
    spinlock_t lock;
} tun_device_t;

void  tun_init(void);
void* tun_open(void);
void  tun_close(void *handle);
int   tun_read(void *handle, void *buf, size_t count);
int   tun_write(void *handle, const void *buf, size_t count);
int   tun_ioctl(void *handle, unsigned long request, void *arg);

#endif
