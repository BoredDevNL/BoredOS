#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "lwip/ethip6.h"
#include "lwip/etharp.h"
#include "netif/etharp.h"
#include "nic.h"
#include "network.h"
#include "kutils.h"

#define IFNAME0 'e'
#define IFNAME1 'n'

static err_t nic_low_level_output(struct netif *netif, struct pbuf *p) {
  (void)netif;

  if (p->next == NULL) {
    if (nic_send_packet(p->payload, p->len) != 0) {
      return ERR_IF;
    }
  } else {
    u8_t buffer[2048];
    u16_t copied = pbuf_copy_partial(p, buffer, 2048, 0);
    if (nic_send_packet(buffer, copied) != 0) {
      return ERR_IF;
    }
  }
  
  LINK_STATS_INC(link.xmit);
  return ERR_OK;
}

static void nic_low_level_input(struct netif *netif) {
  u8_t buffer[2048];
  int len;
  
  while ((len = nic_receive_packet(buffer, sizeof(buffer))) > 0) {
    
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p != NULL) {
      pbuf_take(p, buffer, len);
      if (netif->input(p, netif) != ERR_OK) {
        pbuf_free(p);
      } else {
        LINK_STATS_INC(link.recv);
      }
    }
  }
}

err_t nic_netif_init(struct netif *netif) {
  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  netif->output = etharp_output;
  netif->linkoutput = nic_low_level_output;
  
  nic_driver_t* dev = nic_get_driver();
  if (!dev) return ERR_IF;
  
  netif->hwaddr_len = 6;
  nic_get_mac_address(netif->hwaddr);
  
  netif->mtu = 1500;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
  
  // Explicitly set link to up to trigger lwIP state changes
  netif_set_link_up(netif);
  
  return ERR_OK;
}

void nic_netif_poll(struct netif *netif) {
  nic_low_level_input(netif);
}
