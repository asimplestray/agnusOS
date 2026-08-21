#include <net/net.h>
#include <kheap.h>
#include <pmm.h>
#include <vmm.h>
#include <spinlock.h>
#include <string.h>
#include <serial.h>

struct netif *netif_list = NULL;
struct netif *netif_default = NULL;

void net_init(void) {
    serial_print("ApolloOS: Initializing network stack\n");
    arp_init();
    ip_init();
    icmp_init();
    udp_init();
    serial_print("ApolloOS: Network stack initialized\n");
}

struct netif *netif_alloc(const char *name) {
    struct netif *dev = (struct netif *)kmalloc(sizeof(struct netif));
    if (!dev) return NULL;
    
    memset(dev, 0, sizeof(struct netif));
    strncpy(dev->name, name, 15);
    dev->mtu = ETH_MTU;
    spinlock_init(&dev->lock.lock);
    return dev;
}

int netif_register(struct netif *dev) {
    if (!dev) return -1;
    
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, &flags);
    
    dev->next = netif_list;
    netif_list = dev;
    
    if (!netif_default) {
        netif_default = dev;
    }
    
    spin_unlock_irqrestore(&dev->lock, flags);
    
    serial_print("ApolloOS: Registered netif: ");
    serial_print(dev->name);
    serial_print("\n");
    
    return 0;
}

struct netif *netif_find_by_name(const char *name) {
    struct netif *dev = netif_list;
    while (dev) {
        if (strcmp(dev->name, name) == 0) return dev;
        dev = dev->next;
    }
    return NULL;
}

struct netif *netif_find_by_ip(const uint8_t *ip) {
    struct netif *dev = netif_list;
    while (dev) {
        if (memcmp(dev->ip, ip, 4) == 0) return dev;
        dev = dev->next;
    }
    return NULL;
}

struct net_pkt *pkt_alloc(uint32_t size) {
    struct net_pkt *pkt = (struct net_pkt *)kmalloc(sizeof(struct net_pkt));
    if (!pkt) return NULL;
    
    pkt->data = kmalloc(size);
    if (!pkt->data) {
        kfree(pkt);
        return NULL;
    }
    
    pkt->len = 0;
    pkt->cap = size;
    pkt->dev = NULL;
    pkt->next = NULL;
    return pkt;
}

void pkt_free(struct net_pkt *pkt) {
    if (!pkt) return;
    if (pkt->data) kfree(pkt->data);
    kfree(pkt);
}

void pkt_queue_rx(struct netif *dev, struct net_pkt *pkt) {
    if (!dev || !pkt) return;
    
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, &flags);
    
    pkt->next = dev->rx_queue;
    dev->rx_queue = pkt;
    
    spin_unlock_irqrestore(&dev->lock, flags);
}

struct net_pkt *pkt_dequeue_rx(struct netif *dev) {
    if (!dev) return NULL;
    
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, &flags);
    
    struct net_pkt *pkt = dev->rx_queue;
    if (pkt) {
        dev->rx_queue = pkt->next;
        pkt->next = NULL;
    }
    
    spin_unlock_irqrestore(&dev->lock, flags);
    return pkt;
}

int net_xmit(struct netif *dev, struct net_pkt *pkt) {
    if (!dev || !pkt || !dev->xmit) {
        pkt_free(pkt);
        return -1;
    }
    return dev->xmit(dev, pkt);
}

uint16_t net_checksum(const void *data, uint32_t len) {
    uint32_t sum = 0;
    const uint16_t *ptr = (const uint16_t *)data;
    
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    
    if (len == 1) {
        sum += *(const uint8_t *)ptr;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

uint16_t net_ip_checksum(const struct ip_hdr *iph) {
    uint16_t check = iph->check;
    ((struct ip_hdr *)iph)->check = 0;
    uint16_t calc = net_checksum(iph, (iph->ver_ihl & 0x0F) * 4);
    ((struct ip_hdr *)iph)->check = check;
    return calc;
}