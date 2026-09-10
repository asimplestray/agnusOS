#include <net/net.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>

static struct netif *loopback_dev = NULL;

static int loopback_xmit(struct netif *dev, struct net_pkt *pkt) {
    (void)dev;
    struct eth_hdr *eth = (struct eth_hdr *)pkt->data;
    struct ip_hdr *iph = (struct ip_hdr *)((uint8_t *)eth + ETH_HLEN);
    
    if (ntohs(eth->type) == ETH_P_IP) {
        if (memcmp(iph->daddr, "\x7F\x00\x00\x01", 4) == 0) {
            ip_input(loopback_dev, pkt);
            return 0;
        }
    }
    pkt_free(pkt);
    return 0;
}

struct netif *loopback_init(void) {
    loopback_dev = netif_alloc("lo");
    if (!loopback_dev) return NULL;
    
    uint8_t lo_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t lo_ip[4] = {127, 0, 0, 1};
    uint8_t lo_mask[4] = {255, 0, 0, 0};
    
    memcpy(loopback_dev->mac, lo_mac, ETH_ALEN);
    memcpy(loopback_dev->ip, lo_ip, 4);
    memcpy(loopback_dev->netmask, lo_mask, 4);
    
    loopback_dev->mtu = 16384;
    loopback_dev->flags = NETIF_FLAG_UP | NETIF_FLAG_LOOPBACK;
    loopback_dev->xmit = loopback_xmit;
    
    netif_register(loopback_dev);
    
    serial_print("AgnusOS: Loopback interface initialized (127.0.0.1)\n");
    
    return loopback_dev;
}