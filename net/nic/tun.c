#include "tun.h"
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/ethip6.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "lwip/ip.h"
#include "memory_manager.h"
#include "kutils.h"
#include <string.h>

static tun_device_t tun_dev;
static int tun_initialized = 0;

static err_t tun_low_level_output(struct netif *netif, struct pbuf *p) {
    tun_device_t *tun = (tun_device_t *)netif->state;
    if (!tun || !tun->is_active) return ERR_IF;

    struct pbuf *p_clone = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_POOL);
    if (!p_clone) return ERR_MEM;
    pbuf_copy(p_clone, p);

    if (sockbuf_append(&tun->tx_sb, p_clone, NULL, 0) < 0) {
        return ERR_MEM;
    }
    return ERR_OK;
}

static err_t tun_netif_init(struct netif *netif) {
    netif->name[0] = 't';
    netif->name[1] = 'u';
    netif->output = etharp_output;
    netif->linkoutput = tun_low_level_output;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    netif->hwaddr[0] = 0x02;
    netif->hwaddr[1] = 0x00;
    netif->hwaddr[2] = 0x00;
    netif->hwaddr[3] = 0x00;
    netif->hwaddr[4] = 0x00;
    netif->hwaddr[5] = 0x01;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif_set_link_up(netif);
    return ERR_OK;
}

void tun_init(void) {
    if (tun_initialized) return;
    memset(&tun_dev, 0, sizeof(tun_dev));
    strncpy(tun_dev.name, "tun0", sizeof(tun_dev.name) - 1);
    tun_dev.lock = SPINLOCK_INIT;
    sockbuf_init(&tun_dev.tx_sb, 128 * 1024);
    tun_initialized = 1;
}

void* tun_open(void) {
    if (!tun_initialized) tun_init();
    return &tun_dev;
}

void tun_close(void *handle) {
    (void)handle;
    uint64_t flags = spinlock_acquire_irqsave(&tun_dev.lock);
    if (tun_dev.is_active) {
        netif_set_down(&tun_dev.netif);
        netif_remove(&tun_dev.netif);
        tun_dev.is_active = 0;
    }
    spinlock_release_irqrestore(&tun_dev.lock, flags);
}

int tun_ioctl(void *handle, unsigned long request, void *arg) {
    (void)handle;
    if (!arg) return -1;

    if (request == TUNSETIFF) {
        struct ifreq_tun {
            char ifr_name[16];
            short ifr_flags;
        } *ifr = (struct ifreq_tun *)arg;

        uint64_t flags = spinlock_acquire_irqsave(&tun_dev.lock);
        tun_dev.flags = (uint16_t)ifr->ifr_flags;

        if (!tun_dev.is_active) {
            ip4_addr_t ipaddr, netmask, gw;
            IP4_ADDR(&ipaddr, 10, 0, 0, 1);
            IP4_ADDR(&netmask, 255, 255, 255, 0);
            IP4_ADDR(&gw, 10, 0, 0, 1);

            if (netif_add(&tun_dev.netif, &ipaddr, &netmask, &gw, &tun_dev, tun_netif_init, (tun_dev.flags & IFF_TAP) ? ethernet_input : ip_input) != NULL) {
                netif_set_up(&tun_dev.netif);
                tun_dev.is_active = 1;
                memset(ifr->ifr_name, 0, sizeof(ifr->ifr_name));
                strncpy(ifr->ifr_name, "tun0", sizeof(ifr->ifr_name) - 1);
                spinlock_release_irqrestore(&tun_dev.lock, flags);
                return 0;
            }
        }
        spinlock_release_irqrestore(&tun_dev.lock, flags);
        return 0;
    }
    return -1;
}

int tun_read(void *handle, void *buf, size_t count) {
    (void)handle;
    if (!buf || count == 0) return 0;

    while (sockbuf_is_empty(&tun_dev.tx_sb)) {
        if (!tun_dev.is_active) return -1;
        wait_queue_wait(&tun_dev.tx_sb.waitq);
    }

    return sockbuf_read(&tun_dev.tx_sb, buf, count, NULL, NULL, 0);
}

int tun_write(void *handle, const void *buf, size_t count) {
    (void)handle;
    if (!buf || count == 0 || !tun_dev.is_active) return -1;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)count, PBUF_POOL);
    if (!p) return -1;
    pbuf_take(p, buf, (u16_t)count);

    err_t err;
    if (tun_dev.flags & IFF_TAP) {
        err = ethernet_input(p, &tun_dev.netif);
    } else {
        err = ip_input(p, &tun_dev.netif);
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return -1;
    }
    return (int)count;
}
