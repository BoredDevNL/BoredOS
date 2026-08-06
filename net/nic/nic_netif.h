#ifndef NIC_NETIF_H
#define NIC_NETIF_H

#include "lwip/netif.h"

err_t nic_netif_init(struct netif *netif);
int nic_netif_poll(struct netif *netif);

#endif
