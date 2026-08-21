#include <net/net.h>
#include <kheap.h>
#include <spinlock.h>
#include <string.h>
#include <serial.h>

static uint16_t ip_id_counter = 1;

void ip_init(void) {
    serial_print("ApolloOS: IP initialized\n");
}

uint8_t *ip_local_addr(struct netif *dev) {
    return dev->ip;
}

uint16_t ip_get_id(void) {
    return ip_id_counter++;
}

int ip_output(struct netif *dev, struct net_pkt *pkt, uint8_t proto, const uint8_t *daddr) {
    if (!dev || !pkt) return -1;
    
    struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
    
    iph->ver_ihl = 0x45;
    iph->tos = 0;
    iph->tot_len = htons(pkt->len);
    iph->id = htons(ip_get_id());
    iph->frag_off = 0;
    iph->ttl = 64;
    iph->protocol = proto;
    iph->check = 0;
    memcpy(iph->saddr, dev->ip, 4);
    memcpy(iph->daddr, daddr, 4);
    
    iph->check = net_checksum(iph, sizeof(struct ip_hdr));
    
    uint8_t dest_mac[ETH_ALEN];
    if (memcmp(daddr, dev->ip, 4) == 0) {
        memcpy(dest_mac, dev->mac, ETH_ALEN);
    } else {
        if (arp_resolve(dev, daddr, dest_mac) != 0) {
            return -1;
        }
    }
    
    struct eth_hdr *eth = (struct eth_hdr *)((uint8_t *)pkt->data - ETH_HLEN);
    if ((uint8_t *)eth < (uint8_t *)pkt->data - ETH_HLEN) {
        return -1;
    }
    
    memcpy(eth->dst, dest_mac, ETH_ALEN);
    memcpy(eth->src, dev->mac, ETH_ALEN);
    eth->type = htons(ETH_P_IP);
    
    pkt->len += ETH_HLEN;
    pkt->data = eth;
    
    return net_xmit(dev, pkt);
}

void ip_input(struct netif *dev, struct net_pkt *pkt) {
    struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
    
    if ((iph->ver_ihl >> 4) != 4) {
        pkt_free(pkt);
        return;
    }
    
    uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;
    if (pkt->len < ihl) {
        pkt_free(pkt);
        return;
    }
    
    if (net_ip_checksum(iph) != 0) {
        pkt_free(pkt);
        return;
    }
    
    uint8_t local_ip[4];
    memcpy(local_ip, dev->ip, 4);
    if (memcmp(iph->daddr, local_ip, 4) != 0 && 
        memcmp(iph->daddr, "\xFF\xFF\xFF\xFF", 4) != 0) {
        pkt_free(pkt);
        return;
    }
    
    uint16_t tot_len = ntohs(iph->tot_len);
    if (tot_len > pkt->len) {
        pkt_free(pkt);
        return;
    }
    pkt->len = tot_len;
    
    void *payload = (uint8_t *)iph + ihl;
    uint32_t payload_len = tot_len - ihl;
    
    switch (iph->protocol) {
        case IP_PROTO_ICMP:
            icmp_input(dev, pkt);
            break;
        case IP_PROTO_UDP:
            udp_input(dev, pkt);
            break;
        default:
            pkt_free(pkt);
            break;
    }
}