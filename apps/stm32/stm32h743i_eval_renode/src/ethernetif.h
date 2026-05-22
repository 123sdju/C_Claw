#ifndef CCLAW_STM32H743_ETHERNETIF_H
#define CCLAW_STM32H743_ETHERNETIF_H

#include "lwip/netif.h"

err_t ethernetif_init(struct netif *netif);
void ethernetif_poll(struct netif *netif);

#endif
