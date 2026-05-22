#include "network.h"

#include "board.h"
#include "ethernetif.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include <stdio.h>

#ifndef CCLAW_STM32H743_STATIC_IP
#define CCLAW_STM32H743_STATIC_IP "10.0.2.15"
#endif
#ifndef CCLAW_STM32H743_DNS_SERVER
#define CCLAW_STM32H743_DNS_SERVER "10.0.2.2"
#endif

static struct netif stm32_netif;

static void tcpip_ready(void *arg)
{
    int *done = (int *)arg;
    *done = 1;
}

int network_init_static(void)
{
    board_uart_write("[init] lwIP tcpip_init\n");
    int done = 0;
    tcpip_init(tcpip_ready, &done);
    while (!done) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    if (!ip4addr_aton(CCLAW_STM32H743_STATIC_IP, &ip)) {
        board_uart_write("[fail] invalid static IPv4\n");
        return 0;
    }
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);

    board_uart_write("[init] lwIP netif_add stm32 ethernet\n");
    if (!netif_add(&stm32_netif, &ip, &netmask, &gw, NULL, ethernetif_init, tcpip_input)) {
        board_uart_write("[fail] lwip_netif_add\n");
        return 0;
    }
    netif_set_default(&stm32_netif);
    netif_set_up(&stm32_netif);

    ip4_addr_t dns;
    if (ip4addr_aton(CCLAW_STM32H743_DNS_SERVER, &dns)) {
        dns_setserver(0, &dns);
    }

    char line[128];
    snprintf(line, sizeof(line), "[init] lwIP static IPv4 %s/24 gw 10.0.2.2 dns %s\n",
        CCLAW_STM32H743_STATIC_IP, CCLAW_STM32H743_DNS_SERVER);
    board_uart_write(line);
    board_uart_write("CCLAW_STM32H743_RENODE_NET_PASS\n");
    return 1;
}

void network_poll(void)
{
    ethernetif_poll(&stm32_netif);
}
