#include <rtl8139.h>
#include <pci.h>
#include <io.h>
#include <vmm.h>
#include <pmm.h>
#include <kheap.h>
#include <screen.h>
#include <serial.h>
#include <idt.h>
#include <spinlock.h>
#include <net/net.h>
#include <string.h>

#define RTL8139_MAC0       0x00
#define RTL8139_MAR0       0x08
#define RTL8139_TX_ADDR0   0x20
#define RTL8139_TX_STATUS0 0x10
#define RTL8139_RX_BUF     0x30
#define RTL8139_RX_BUF_PTR 0x38
#define RTL8139_RX_BUF_END 0x3A
#define RTL8139_ERBCR      0x3C
#define RTL8139_ERBCR_VAL  0x00
#define RTL8139_RCR        0x44
#define RTL8139_TCR        0x40
#define RTL8139_IMR        0x3C
#define RTL8139_ISR        0x3E
#define RTL8139_9346CR     0x50
#define RTL8139_CONFIG0    0x51
#define RTL8139_CONFIG1    0x52
#define RTL8139_MULTI_INTR 0x5C
#define RTL8139_CHIP_CMD   0x37

#define RTL8139_CMD_RESET  0x10
#define RTL8139_CMD_RX_EN  0x08
#define RTL8139_CMD_TX_EN  0x04

#define RTL8139_RCR_AAP    0x00000001
#define RTL8139_RCR_APM    0x00000002
#define RTL8139_RCR_AM     0x00000004
#define RTL8139_RCR_AB     0x00000008
#define RTL8139_RCR_WRAP   0x00000010
#define RTL8139_RCR_MXDMA  0x00000700
#define RTL8139_RCR_RBLEN  0x00001800
#define RTL8139_RCR_RFIFO  0x0000E000

#define RTL8139_TCR_IFG    0x03
#define RTL8139_TCR_MXDMA  0x700
#define RTL8139_TCR_TXRR   0x00F00000
#define RTL8139_TCR_CLRABT 0x01000000

#define RTL8139_ISR_ROK    0x0001
#define RTL8139_ISR_RER    0x0002
#define RTL8139_ISR_TOK    0x0004
#define RTL8139_ISR_TER    0x0008
#define RTL8139_ISR_RX_FIFO_OVW 0x0010
#define RTL8139_ISR_SYSTEM_ERR 0x0080

#define RTL8139_9346CR_EEM0 0x01
#define RTL8139_9346CR_EEM1 0x02
#define RTL8139_9346CR_EEM  0x03

#define RX_BUF_SIZE        8192
#define RX_BUF_WRAP_PAD    16
#define TX_BUF_SIZE        2048
#define NUM_TX_DESC        4

static struct rtl8139_dev *rtl8139_device = NULL;

static uint8_t rtl8139_read8(struct rtl8139_dev *dev, uint8_t reg) {
    return inb(dev->io_base + reg);
}

static uint16_t rtl8139_read16(struct rtl8139_dev *dev, uint8_t reg) {
    return inw(dev->io_base + reg);
}

static uint32_t rtl8139_read32(struct rtl8139_dev *dev, uint8_t reg) {
    return inl(dev->io_base + reg);
}

static void rtl8139_write8(struct rtl8139_dev *dev, uint8_t reg, uint8_t val) {
    outb(dev->io_base + reg, val);
}

static void rtl8139_write16(struct rtl8139_dev *dev, uint8_t reg, uint16_t val) {
    outw(dev->io_base + reg, val);
}

static void rtl8139_write32(struct rtl8139_dev *dev, uint8_t reg, uint32_t val) {
    outl(dev->io_base + reg, val);
}

static void rtl8139_reset(struct rtl8139_dev *dev) {
    rtl8139_write8(dev, RTL8139_CHIP_CMD, RTL8139_CMD_RESET);
    while (rtl8139_read8(dev, RTL8139_CHIP_CMD) & RTL8139_CMD_RESET) {
        __asm__ volatile("pause");
    }
}

static void rtl8139_hw_init(struct rtl8139_dev *dev) {
    rtl8139_write8(dev, RTL8139_9346CR, RTL8139_9346CR_EEM);
    rtl8139_write8(dev, RTL8139_CONFIG1, 0x00);
    rtl8139_write8(dev, RTL8139_9346CR, 0x00);
    
    rtl8139_reset(dev);
    
    rtl8139_write32(dev, RTL8139_RX_BUF, (uint32_t)dev->rx_buf_phys);
    
    rtl8139_write32(dev, RTL8139_RCR, 
        RTL8139_RCR_APM | RTL8139_RCR_AM | RTL8139_RCR_AB |
        RTL8139_RCR_WRAP | (0x03 << 11) | (0x03 << 13));
    
    rtl8139_write32(dev, RTL8139_TCR,
        RTL8139_TCR_IFG | (0x03 << 8) | (0x0F << 20));
    
    for (int i = 0; i < NUM_TX_DESC; i++) {
        rtl8139_write32(dev, RTL8139_TX_ADDR0 + i * 4, (uint32_t)dev->tx_buf_phys[i]);
    }
    
    rtl8139_write16(dev, RTL8139_IMR, 
        RTL8139_ISR_ROK | RTL8139_ISR_TOK | RTL8139_ISR_RER | RTL8139_ISR_TER);
    
    rtl8139_write8(dev, RTL8139_CHIP_CMD, 
        RTL8139_CMD_RX_EN | RTL8139_CMD_TX_EN);
}

static void rtl8139_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    if (!rtl8139_device) return;
    
    struct rtl8139_dev *dev = rtl8139_device;
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, &flags);
    
    uint16_t isr = rtl8139_read16(dev, RTL8139_ISR);
    rtl8139_write16(dev, RTL8139_ISR, isr);
    
    if (isr & RTL8139_ISR_ROK) {
        uint32_t rx_status = rtl8139_read32(dev, RTL8139_RX_BUF);
        uint16_t rx_len = rx_status >> 16;
        
        if (rx_len > 4 && rx_len < RX_BUF_SIZE) {
            struct net_pkt *pkt = pkt_alloc(rx_len);
            if (pkt) {
                memcpy(pkt->data, (void *)((uintptr_t)dev->rx_buf_virt + dev->rx_buf_len), rx_len);
                pkt->len = rx_len;
                if (dev->netif && dev->netif->rx_handler) {
                    dev->netif->rx_handler(dev->netif, pkt->data, rx_len);
                } else {
                    pkt_free(pkt);
                }
            }
        }
        
        dev->rx_buf_len = (dev->rx_buf_len + rx_len + 4 + 3) & ~3;
        if (dev->rx_buf_len >= RX_BUF_SIZE) {
            dev->rx_buf_len = 0;
        }
        rtl8139_write16(dev, RTL8139_RX_BUF_PTR, dev->rx_buf_len - 16);
    }
    
    if (isr & RTL8139_ISR_TOK) {
    }
    
    spin_unlock_irqrestore(&dev->lock, flags);
}

static int rtl8139_send(struct rtl8139_dev *dev, const void *data, uint16_t len) {
    if (!dev || len > TX_BUF_SIZE) return -1;
    
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, &flags);
    
    static int tx_idx = 0;
    void *tx_buf = dev->tx_buf_virt[tx_idx];
    memcpy(tx_buf, data, len);
    
    rtl8139_write32(dev, RTL8139_TX_STATUS0 + tx_idx * 4, len);
    
    tx_idx = (tx_idx + 1) % NUM_TX_DESC;
    
    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

int rtl8139_init(struct rtl8139_dev *dev, uint8_t bus, uint8_t dev_num, uint8_t func) {
    dev->bus = bus;
    dev->dev = dev_num;
    dev->func = func;
    
    uint16_t vendor = pci_read_word(bus, dev_num, func, PCI_CONFIG_VENDOR_ID);
    uint16_t device = pci_read_word(bus, dev_num, func, PCI_CONFIG_DEVICE_ID);
    
    if (vendor != RTL8139_VENDOR_ID || device != RTL8139_DEVICE_ID) {
        return -1;
    }
    
    pci_write_word(bus, dev_num, func, PCI_CONFIG_COMMAND, 
        PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
    
    uint32_t bar0 = pci_read_bar(bus, dev_num, func, 0);
    dev->io_base = bar0 & 0xFFFC;
    
    uint8_t irq = pci_read_word(bus, dev_num, func, PCI_CONFIG_INTERRUPT_LINE) & 0xFF;
    dev->irq = irq;
    
    dev->rx_buf_phys = pmm_alloc_block();
    dev->rx_buf_virt = (void *)((uintptr_t)dev->rx_buf_phys + 0xFFFF800000000000ULL);
    dev->rx_buf_len = 0;
    
    for (int i = 0; i < NUM_TX_DESC; i++) {
        dev->tx_buf_phys[i] = pmm_alloc_block();
        dev->tx_buf_virt[i] = (void *)((uintptr_t)dev->tx_buf_phys[i] + 0xFFFF800000000000ULL);
    }
    
    spinlock_init(&dev->lock.lock);
    
    rtl8139_hw_init(dev);
    
    for (int i = 0; i < 6; i++) {
        dev->mac_addr[i] = rtl8139_read8(dev, RTL8139_MAC0 + i);
    }
    
    interrupts_register_handler(irq + 32, rtl8139_irq_handler);
    
    rtl8139_device = dev;
    
    serial_print("AgnusOS: RTL8139 initialized at IO 0x");
    char hex[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = hex[(dev->io_base >> 12) & 0xF];
    buf[1] = hex[(dev->io_base >> 8) & 0xF];
    buf[2] = hex[(dev->io_base >> 4) & 0xF];
    buf[3] = hex[dev->io_base & 0xF];
    buf[4] = '\0';
    serial_print(buf);
    serial_print(", IRQ ");
    buf[0] = hex[(irq >> 4) & 0xF];
    buf[1] = hex[irq & 0xF];
    buf[2] = '\0';
    serial_print(buf);
    serial_print("\n");
    
    return 0;
}

int rtl8139_xmit(struct netif *dev, struct net_pkt *pkt) {
    if (!dev || !pkt) return -1;
    struct rtl8139_dev *rtl_dev = (struct rtl8139_dev *)dev;
    return rtl8139_send(rtl_dev, pkt->data, pkt->len);
}

void rtl8139_get_mac(struct rtl8139_dev *dev, uint8_t *mac) {
    memcpy(mac, dev->mac_addr, 6);
}

void rtl8139_set_rx_handler(struct rtl8139_dev *dev, struct netif *netif) {
    dev->netif = netif;
}