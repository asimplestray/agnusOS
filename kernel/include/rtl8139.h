#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <idt.h>
#include <net/net.h>

#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

struct rtl8139_dev {
    uint8_t bus, dev, func;
    uint16_t io_base;
    uint64_t rx_buf_phys;
    void *rx_buf_virt;
    uint16_t rx_buf_len;
    uint64_t tx_buf_phys[4];
    void *tx_buf_virt[4];
    uint8_t mac_addr[6];
    uint8_t irq;
    spinlock_irq_t lock;
    struct netif *netif;
};

int rtl8139_init(struct rtl8139_dev *dev, uint8_t bus, uint8_t dev_num, uint8_t func);
int rtl8139_xmit(struct netif *dev, struct net_pkt *pkt);
void rtl8139_get_mac(struct rtl8139_dev *dev, uint8_t *mac);
void rtl8139_set_rx_handler(struct rtl8139_dev *dev, struct netif *netif);

#endif