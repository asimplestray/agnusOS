#include <net/net.h>
#include <kheap.h>
#include <spinlock.h>
#include <string.h>
#include <serial.h>

void icmp_init(void) {
    serial_print("AgnusOS: ICMP initialized\n");
}

int icmp_send_echo_reply(struct netif *dev, const struct ip_hdr *iph, const struct icmp_hdr *icmph) {
    struct net_pkt *pkt = pkt_alloc(ETH_HLEN + sizeof(struct ip_hdr) + sizeof(struct icmp_hdr) + 
                                    (ntohs(iph->tot_len) - ((iph->ver_ihl & 0x0F) * 4) - sizeof(struct icmp_hdr)));
    if (!pkt) return -1;
    
    struct ip_hdr *iph_reply = (struct ip_hdr *)(pkt->data + ETH_HLEN);
    struct icmp_hdr *icmph_reply = (struct icmp_hdr *)((uint8_t *)iph_reply + sizeof(struct ip_hdr));
    
    uint16_t payload_len = ntohs(iph->tot_len) - ((iph->ver_ihl & 0x0F) * 4) - sizeof(struct icmp_hdr);
    if (payload_len > 0) {
        memcpy((uint8_t *)icmph_reply + sizeof(struct icmp_hdr), 
               (uint8_t *)icmph + sizeof(struct icmp_hdr), payload_len);
    }
    
    icmph_reply->type = ICMP_ECHO_REPLY;
    icmph_reply->code = 0;
    icmph_reply->check = 0;
    icmph_reply->id = icmph->id;
    icmph_reply->seq = icmph->seq;
    
    uint16_t icmp_len = sizeof(struct icmp_hdr) + payload_len;
    icmph_reply->check = net_checksum(icmph_reply, icmp_len);
    
    iph_reply->ver_ihl = 0x45;
    iph_reply->tos = 0;
    iph_reply->tot_len = htons(sizeof(struct ip_hdr) + icmp_len);
    iph_reply->id = htons(ip_get_id());
    iph_reply->frag_off = 0;
    iph_reply->ttl = 64;
    iph_reply->protocol = IP_PROTO_ICMP;
    iph_reply->check = 0;
    memcpy(iph_reply->saddr, iph->daddr, 4);
    memcpy(iph_reply->daddr, iph->saddr, 4);
    iph_reply->check = net_checksum(iph_reply, sizeof(struct ip_hdr));
    
    pkt->len = ETH_HLEN + sizeof(struct ip_hdr) + icmp_len;
    
    return ip_output(dev, pkt, IP_PROTO_ICMP, iph->saddr);
}

void icmp_input(struct netif *dev, struct net_pkt *pkt) {
    struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
    uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;
    struct icmp_hdr *icmph = (struct icmp_hdr *)((uint8_t *)iph + ihl);
    
    if (pkt->len < ihl + sizeof(struct icmp_hdr)) {
        pkt_free(pkt);
        return;
    }
    
    if (net_checksum(icmph, pkt->len - ihl) != 0) {
        pkt_free(pkt);
        return;
    }
    
    switch (icmph->type) {
        case ICMP_ECHO_REQUEST:
            icmp_send_echo_reply(dev, iph, icmph);
            break;
        default:
            break;
    }
    
    pkt_free(pkt);
}