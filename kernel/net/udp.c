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
    serial_print("AgnusOS: UDP initialized\n");
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
            udp_socks[i].closed = 0;
            udp_socks[i].rx_queue = NULL;
            udp_socks[i].rx_tail = NULL;
            udp_socks[i].rx_count = 0;
            udp_socks[i].rx_drops = 0;
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
    /* data/len reservados p/ validação RX futura (ordem de rede); o TX
     * deriva tudo de uh->len. */
    (void)data;
    (void)len;
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
    if (!sock || !buf || len == 0) return -1;

    for (;;) {
        unsigned long flags;
        spin_lock_irqsave(&sock->lock, &flags);
        if (sock->closed) {
            spin_unlock_irqrestore(&sock->lock, flags);
            return -1;
        }
        if (sock->rx_queue) {
            struct net_pkt *pkt = sock->rx_queue;
            sock->rx_queue = pkt->next;
            if (!sock->rx_queue)
                sock->rx_tail = NULL;
            if (pkt->next)
                pkt->next = NULL;
            sock->rx_count--;
            spin_unlock_irqrestore(&sock->lock, flags);

            /* Validate before touching payload: corrupt length fields
             * arrive from the wire and must not cause underflow/OOB. */
            struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
            uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;
            if (ihl < 20 || pkt->len < (uint32_t)(ihl + sizeof(struct udp_hdr))) {
                pkt_free(pkt);
                return -1;
            }
            struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)iph + ihl);
            uint32_t udp_len = ntohs(uh->len);
            if (udp_len < sizeof(struct udp_hdr) ||
                udp_len > pkt->len - ihl) {
                pkt_free(pkt);
                return -1;
            }
            uint32_t payload_len = udp_len - sizeof(struct udp_hdr);
            void *payload = (uint8_t *)uh + sizeof(struct udp_hdr);

            uint32_t copy_len = len < payload_len ? len : payload_len;
            memcpy(buf, payload, copy_len);

            if (src_ip) memcpy(src_ip, iph->saddr, 4);
            if (src_port) *src_port = ntohs(uh->src_port);

            int ret = (int)copy_len;
            pkt_free(pkt);
            return ret;
        }
        spin_unlock_irqrestore(&sock->lock, flags);

        /* Sleep until data or close. Close sets closed + wake_up, so a
         * receiver blocked here always wakes with -1 instead of hanging
         * on a freed/reused slot. */
        wait_event(sock->wait, sock->rx_queue != NULL || sock->closed);
    }
}

void udp_close(struct udp_sock *sock) {
    if (!sock) return;

    unsigned long flags;
    spin_lock_irqsave(&sock->lock, &flags);
    sock->closed = 1;
    while (sock->rx_queue) {
        struct net_pkt *pkt = sock->rx_queue;
        sock->rx_queue = pkt->next;
        pkt_free(pkt);
    }
    sock->rx_tail = NULL;
    sock->rx_count = 0;
    sock->bound = 0;
    wake_up(&sock->wait);
    spin_unlock_irqrestore(&sock->lock, flags);
}

void udp_input(struct netif *dev, struct net_pkt *pkt) {
    (void)dev;
    struct ip_hdr *iph = (struct ip_hdr *)pkt->data;
    uint8_t ihl = (iph->ver_ihl & 0x0F) * 4;
    /* Length fields come from the wire: validate before dereferencing. */
    if (ihl < 20 || pkt->len < (uint32_t)(ihl + sizeof(struct udp_hdr))) {
        pkt_free(pkt);
        return;
    }
    struct udp_hdr *uh = (struct udp_hdr *)((uint8_t *)iph + ihl);
    uint32_t udp_len = ntohs(uh->len);
    if (udp_len < sizeof(struct udp_hdr) ||
        udp_len > pkt->len - ihl) {
        pkt_free(pkt);
        return;
    }
    /* RX checksum is not validated (TX helper sums in host order; proper
     * network-order validation is future work). Lengths above are the
     * memory-safety boundary. */

    uint16_t dst_port = ntohs(uh->dst_port);

    unsigned long outer_flags;
    spin_lock_irqsave(&udp_lock, &outer_flags);

    for (int i = 0; i < MAX_UDP_SOCKS; i++) {
        if (udp_socks[i].bound && !udp_socks[i].closed &&
            udp_socks[i].sport == dst_port) {
            struct udp_sock *sock = &udp_socks[i];

            /* Nested locks need separate saved states: sharing one
             * flags variable corrupts the outer IRQ restore. */
            unsigned long inner_flags;
            spin_lock_irqsave(&sock->lock, &inner_flags);
            if (sock->closed || sock->rx_count >= UDP_MAX_RX_QUEUE) {
                if (!sock->closed)
                    sock->rx_drops++;
                spin_unlock_irqrestore(&sock->lock, inner_flags);
                spin_unlock_irqrestore(&udp_lock, outer_flags);
                pkt_free(pkt);
                return;
            }
            /* FIFO tail-append (was head-insert = LIFO). */
            pkt->next = NULL;
            if (sock->rx_tail)
                sock->rx_tail->next = pkt;
            else
                sock->rx_queue = pkt;
            sock->rx_tail = pkt;
            sock->rx_count++;
            wake_up(&sock->wait);
            spin_unlock_irqrestore(&sock->lock, inner_flags);

            spin_unlock_irqrestore(&udp_lock, outer_flags);
            return;
        }
    }

    spin_unlock_irqrestore(&udp_lock, outer_flags);
    pkt_free(pkt);
}