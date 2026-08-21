#include <net/net.h>
#include <kheap.h>
#include <spinlock.h>
#include <string.h>
#include <serial.h>
#include <task.h>
#include <wait.h>

#define MAX_UDP_SOCKS  32

static struct udp_sock udp_socks[MAX_UDP_SOCKS];
static spinlock_irq_t udp_lock = { SPINLOCK_INIT, 0 };

void udp_init(void) {
    memset(udp_socks, 0, sizeof(udp_socks));
    serial_print("ApolloOS: UDP initialized\n");
}

struct udp_sock *udp_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    
    unsigned long flags;
    spin_lock_irqsave(&udp_lock, &flags);
    
    for (int i = 0; i < MAX_UDP_SOCKS; i++) {
        if (!udp_socks[i].bound) {
            memset(&udp_socks[i], 0, sizeof(struct udp_sock));
            spinlock_init(&udp_socks[i].lock.lock);
            init_waitqueue_head(&udp_socks[i].wait);
            udp_socks[i].bound = 1;
            spin_unlock_irqrestore(&udp_lock, flags);
            return &udp_socks[i];
        }
    }
    
    spin_unlock_irqrestore(&udp_lock, flags);
    return NULL;
}

int udp_bind(struct udp_sock *sock, const uint8_t *addr, uint16_t port) {
    if (!sock) return -1;
    
    unsigned long flags;
    spin_lock_irqsave(&sock->lock, &flags);
    
    sock->sport = port;
    if (addr) memcpy(sock->dst_ip, addr, 4);
    
    spin_unlock_irqrestore(&sock->lock, flags);
    return 0;
}

static uint16_t udp_checksum_pseudo(const struct ip_hdr *iph, const struct udp_hdr *uh, const void *data, uint32_t len) {
    uint32_t sum = 0;
    
    sum += (iph->saddr[0] << 8) | iph->saddr[1];
    sum += (iph->saddr[2] << 8) | iph->saddr[3];
    sum += (iph->daddr[0] << 8) | iph->daddr[1];
    sum += (iph->daddr[2] << 8) | iph->daddr[3];
    sum += htons(IP_PROTO_UDP);
    sum += htons(ntohs(uh->len));
    
    const uint16_t *ptr = (const uint16_t *)uh;
    uint32_t udp_len = ntohs(uh->len);
    while (udp_len > 1) {
        sum += *ptr++;
        udp_len -= 2;
    }
    if (udp_len == 1) {
        sum += *(const uint8_t *)ptr;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

int udp_sendto(struct udp_sock *sock, const void *data, uint32_t len, const uint8_t *dst_ip, uint16_t dst_port) {
    if (!sock || !sock->dev) return -1;
    
    struct netif *dev = sock->dev;
    struct net_pkt *pkt = pkt_alloc(ETH_HLEN + sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + len);
    if (!pkt) return -1;
    
    struct ip_hdr *iph = (struct ip_hdr *)(pkt->data + ETH_HLEN);
    struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)iph + sizeof(struct ip_hdr));
    void *payload = (uint8_t *)uh + sizeof(struct udp_hdr);
    
    memcpy(payload, data, len);
    
    uh->src_port = htons(sock->sport);
    uh->dst_port = htons(dst_port);
    uh->len = htons(sizeof(struct udp_hdr) + len);
    uh->check = 0;
    
    iph->ver_ihl = 0x45;
    iph->tos = 0;
    iph->tot_len = htons(sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + len);
    iph->id = htons(ip_get_id());
    iph->frag_off = 0;
    iph->ttl = 64;
    iph->protocol = IP_PROTO_UDP;
    iph->check = 0;
    memcpy(iph->saddr, dev->ip, 4);
    memcpy(iph->daddr, dst_ip, 4);
    
    uh->check = udp_checksum_pseudo(iph, uh, payload, len);
    iph->check = net_checksum(iph, sizeof(struct ip_hdr));
    
    pkt->len = ETH_HLEN + sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + len;
    
    return ip_output(dev, pkt, IP_PROTO_UDP, dst_ip);
}

int udp_recvfrom(struct udp_sock *sock, void *buf, uint32_t len, uint8_t *src_ip, uint16_t *src_port) {
    if (!sock) return -1;
    
    unsigned long flags;
    spin_lock_irqsave(&sock->lock, &flags);
    
    if (!sock->rx_queue) {
        spin_unlock_irqrestore(&sock->lock, flags);
        
        wait_event(sock->wait, sock->rx_queue != NULL);
        
        spin_lock_irqsave(&sock->lock, &flags);
    }
    
    struct net_pkt *pkt = sock->rx_queue;
    if (!pkt) {
        spin_unlock_irqrestore(&sock->lock, flags);
        return 0;
    }
    
    sock->rx_queue = pkt->next;
    spin_unlock_irqrestore(&sock->lock, flags);
    
    struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
    uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;
    struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)iph + ihl);
    void *payload = (uint8_t *)uh + sizeof(struct udp_hdr);
    uint32_t payload_len = ntohs(uh->len) - sizeof(struct udp_hdr);
    
    uint32_t copy_len = len < payload_len ? len : payload_len;
    memcpy(buf, payload, copy_len);
    
    if (src_ip) memcpy(src_ip, iph->saddr, 4);
    if (src_port) *src_port = ntohs(uh->src_port);
    
    int ret = copy_len;
    pkt_free(pkt);
    
    return ret;
}

void udp_close(struct udp_sock *sock) {
    if (!sock) return;
    
    unsigned long flags;
    spin_lock_irqsave(&sock->lock, &flags);
    
    while (sock->rx_queue) {
        struct net_pkt *pkt = sock->rx_queue;
        sock->rx_queue = pkt->next;
        pkt_free(pkt);
    }
    
    sock->bound = 0;
    spin_unlock_irqrestore(&sock->lock, flags);
}

void udp_input(struct netif *dev, struct net_pkt *pkt) {
    struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
    uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;
    struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)iph + ihl);
    
    if (pkt->len < ihl + sizeof(struct udp_hdr)) {
        pkt_free(pkt);
        return;
    }
    
    uint16_t dst_port = ntohs(uh->dst_port);
    
    unsigned long flags;
    spin_lock_irqsave(&udp_lock, &flags);
    
    for (int i = 0; i < MAX_UDP_SOCKS; i++) {
        if (udp_socks[i].bound && udp_socks[i].sport == dst_port) {
            struct udp_sock *sock = &udp_socks[i];
            
            spin_lock_irqsave(&sock->lock, &flags);
            pkt->next = sock->rx_queue;
            sock->rx_queue = pkt;
            wake_up(&sock->wait);
            spin_unlock_irqrestore(&sock->lock, flags);
            
            spin_unlock_irqrestore(&udp_lock, flags);
            return;
        }
    }
    
    spin_unlock_irqrestore(&udp_lock, flags);
    pkt_free(pkt);
}