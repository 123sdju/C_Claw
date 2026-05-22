#include "ethernetif.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "stm32h7xx_hal.h"

#define ETH_RX_BUFFER_SIZE 1536U

typedef struct rx_packet {
    uint8_t data[ETH_RX_BUFFER_SIZE] __attribute__((aligned(32)));
    uint8_t in_use;
} rx_packet_t;

static ETH_DMADescTypeDef rx_desc[ETH_RX_DESC_CNT] __attribute__((section(".RxDescripSection"), aligned(32)));
static ETH_DMADescTypeDef tx_desc[ETH_TX_DESC_CNT] __attribute__((section(".TxDescripSection"), aligned(32)));
static rx_packet_t rx_packets[ETH_RX_DESC_CNT] __attribute__((section(".RxPoolSection"), aligned(32)));
static uint8_t tx_buffer[ETH_RX_BUFFER_SIZE] __attribute__((section(".TxPoolSection"), aligned(32)));

static ETH_HandleTypeDef eth_handle;
static uint8_t mac_addr[6] = {
    ETH_MAC_ADDR0, ETH_MAC_ADDR1, ETH_MAC_ADDR2, ETH_MAC_ADDR3, ETH_MAC_ADDR4, ETH_MAC_ADDR5
};

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
    for (uint32_t i = 0; i < ETH_RX_DESC_CNT; i++) {
        if (!rx_packets[i].in_use) {
            rx_packets[i].in_use = 1;
            *buff = rx_packets[i].data;
            return;
        }
    }
    *buff = NULL;
}

void HAL_ETH_RxLinkCallback(void **p_start, void **p_end, uint8_t *buff, uint16_t len)
{
    for (uint32_t i = 0; i < ETH_RX_DESC_CNT; i++) {
        if (rx_packets[i].data == buff) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
            if (p) pbuf_take(p, buff, len);
            rx_packets[i].in_use = 0;
            if (!p) return;
            if (*p_start == NULL) {
                *p_start = p;
                *p_end = p;
            } else {
                ((struct pbuf *)(*p_end))->next = p;
                *p_end = p;
            }
            return;
        }
    }
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if (p->tot_len > sizeof(tx_buffer)) return ERR_BUF;

    uint8_t *cursor = tx_buffer;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(cursor, q->payload, q->len);
        cursor += q->len;
    }

    ETH_BufferTypeDef buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.buffer = tx_buffer;
    buffer.len = p->tot_len;

    ETH_TxPacketConfig config;
    memset(&config, 0, sizeof(config));
    config.Length = p->tot_len;
    config.TxBuffer = &buffer;
    config.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    config.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    config.CRCPadCtrl = ETH_CRC_PAD_INSERT;

    if (HAL_ETH_Transmit(&eth_handle, &config, 100U) != HAL_OK) {
        return ERR_IF;
    }
    return ERR_OK;
}

err_t ethernetif_init(struct netif *netif)
{
    netif->name[0] = 's';
    netif->name[1] = 't';
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac_addr, sizeof(mac_addr));
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    memset(&eth_handle, 0, sizeof(eth_handle));
    eth_handle.Instance = ETH;
    eth_handle.Init.MACAddr = mac_addr;
    eth_handle.Init.MediaInterface = HAL_ETH_RMII_MODE;
    eth_handle.Init.RxDesc = rx_desc;
    eth_handle.Init.TxDesc = tx_desc;
    eth_handle.Init.RxBuffLen = ETH_RX_BUFFER_SIZE;

    if (HAL_ETH_Init(&eth_handle) != HAL_OK) {
        return ERR_IF;
    }

    ETH_MACConfigTypeDef mac_config;
    HAL_ETH_GetMACConfig(&eth_handle, &mac_config);
    mac_config.DuplexMode = ETH_FULLDUPLEX_MODE;
    mac_config.Speed = ETH_SPEED_100M;
    mac_config.ChecksumOffload = ENABLE;
    HAL_ETH_SetMACConfig(&eth_handle, &mac_config);

    if (HAL_ETH_Start(&eth_handle) != HAL_OK) {
        return ERR_IF;
    }
    netif_set_link_up(netif);
    return ERR_OK;
}

void ethernetif_poll(struct netif *netif)
{
    for (;;) {
        void *packet = NULL;
        if (HAL_ETH_ReadData(&eth_handle, &packet) != HAL_OK || !packet) break;
        struct pbuf *p = (struct pbuf *)packet;
        (void)netif->input(p, netif);
    }
}
