#include <net/net.h>
#include <kheap.h>
#include <spinlock.h>
#include <string.h>
#include <serial.h>
#include <timer.h>

#define ARP_TABLE_SIZE  16
#define ARP_TIMEOUT     30000

struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[ETH_ALEN];
    uint32_t expires;
    uint8_t state;
    struct netif *dev;
};

static struct arp_entry arp_table[ARP_TABLE_SIZE];
static spinlock_irq_t arp_lock = { SPINLOCK_INIT, 0 };

void arp_init(void) {
    memset(arp_table, 0, sizeof(arp_table));
    serial_print("ApolloOS: ARP initialized\n");
}

static struct arp_entry *arp_find(const uint8_t *ip) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].state && memcmp(arp_table[i].ip, ip, 4) == 0) {
            return &arp_table[i];
        }
    }
    return NULL;
}

static struct arp_entry *arp_alloc(const uint8_t *ip) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].state) {
            arp_table[i].state = 1;
            memcpy(arp_table[i].ip, ip, 4);
            return &arp_table[i];
        }
    }
    return NULL;
}

void arp_update(const uint8_t *sender_ip, const uint8_t *sender_mac) {
    unsigned long flags;
    spin_lock_irqsave(&arp_lock, &flags);
    
    struct arp_entry *e = arp_find(sender_ip);
    if (!e) e = arp_alloc(sender_ip);
    if (e) {
        memcpy(e->mac, sender_mac, ETH_ALEN);
        e->expires = timer_get_ticks() + ARP_TIMEOUT;
    }
    
    spin_unlock_irqrestore(&arp_lock, flags);
}

int arp_request(struct netif *dev, const uint8_t *target_ip) {
    struct net_pkt *pkt = pkt_alloc(ETH_HLEN + sizeof(struct arp_hdr));
    if (!pkt) return -1;
    
    struct eth_hdr *eth = (struct eth_hdr *)pkt->data;
    struct arp_hdr *arp = (struct arp_hdr *)(eth + 1);
    
    memset(eth->dst, 0xFF, ETH_ALEN);
    memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->type = htons(ETH_P_ARP);
    
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ETH_P_IP);
    arp->hlen = ETH_ALEN;
    arp->plen = 4;
    arp->op = htons(ARP_OP_REQUEST);
    memcpy(arp->sha, dev->mac, ETH_ALEN);
    memcpy(arp->spa, dev->ip, 4);
    memset(arp->tha, 0, ETH_ALEN);
    memcpy(arp->tpa, target_ip, 4);
    
    pkt->len = ETH_HLEN + sizeof(struct arp_hdr);
    pkt->dev = dev;
    
    return net_xmit(dev, pkt);
}

void arp_reply(struct netif *dev, const struct arp_hdr *req) {
    struct net_pkt *pkt = pkt_alloc(ETH_HLEN + sizeof(struct arp_hdr));
    if (!pkt) return;
    
    struct eth_hdr *eth = (struct eth_hdr *)pkt->data;
    struct arp_hdr *arp = (struct arp_hdr *)(eth + 1);
    
    memcpy(eth->dst, req->sha, ETH_ALEN);
    memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->type = htons(ETH_P_ARP);
    
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ETH_P_IP);
    arp->hlen = ETH_ALEN;
    arp->plen = 4;
    arp->op = htons(ARP_OP_REPLY);
    memcpy(arp->sha, dev->mac, ETH_ALEN);
    memcpy(arp->spa, dev->ip, 4);
    memcpy(arp->tha, req->sha, ETH_ALEN);
    memcpy(arp->tpa, req->spa, 4);
    
    pkt->len = ETH_HLEN + sizeof(struct arp_hdr);
    pkt->dev = dev;
    
    net_xmit(dev, pkt);
}

int arp_resolve(struct netif *dev, const uint8_t *ip, uint8_t *mac) {
    unsigned long flags;
    spin_lock_irqsave(&arp_lock, &flags);
    
    struct arp_entry *e = arp_find(ip);
    if (e && e->expires > timer_get_ticks()) {
        memcpy(mac, e->mac, ETH_ALEN);
        spin_unlock_irqrestore(&arp_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&arp_lock, flags);
    
    return arp_request(dev, ip);
}

/* Handle incoming ARP packet from the wire */
void arp_input(struct netif *dev, void *data, uint32_t len) {
    if (!dev || !data || len < sizeof(struct arp_hdr)) return;

    struct arp_hdr *arph = (struct arp_hdr *)data;

    /* Only handle Ethernet/IP ARP */
    if (ntohs(arph->htype) != ARP_HTYPE_ETH ||
        ntohs(arph->ptype) != ETH_P_IP ||
        arph->hlen != ETH_ALEN || arph->plen != 4)
        return;

    /* Learn sender's mapping from any ARP packet */
    arp_update(arph->spa, arph->sha);

    /* If it's an ARP request for our IP, send a reply */
    if (ntohs(arph->op) == ARP_OP_REQUEST) {
        if (memcmp(arph->tpa, dev->ip, 4) == 0) {
            arp_reply(dev, arph);
        }
    }
}