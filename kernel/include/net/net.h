#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <spinlock.h>
#include <wait.h>

static inline uint16_t htons(uint16_t x) { return (x << 8) | (x >> 8); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) { return (x << 24) | ((x << 8) & 0xFF0000) | ((x >> 8) & 0xFF00) | (x >> 24); }
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500
#define ETH_FRAME_LEN   (ETH_MTU + ETH_HLEN + 4)

#define ETH_P_IP        0x0800
#define ETH_P_ARP       0x0806

#define ARP_HTYPE_ETH   1
#define ARP_PTYPE_IP    0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

#define UDP_HLEN          8
#define TCP_HLEN          20

#define MAX_PACKET_SIZE   2048

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed));

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t op;
    uint8_t  sha[ETH_ALEN];
    uint8_t  spa[4];
    uint8_t  tha[ETH_ALEN];
    uint8_t  tpa[4];
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint8_t  saddr[4];
    uint8_t  daddr[4];
} __attribute__((packed));

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t check;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t check;
} __attribute__((packed));

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t doff_flags;
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
} __attribute__((packed));

struct net_pkt {
    void *data;
    uint32_t len;
    uint32_t cap;
    struct netif *dev;
    struct net_pkt *next;
};

struct netif {
    char name[16];
    uint8_t mac[ETH_ALEN];
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gw[4];
    uint32_t mtu;
    uint32_t flags;
    
    int (*xmit)(struct netif *, struct net_pkt *);
    void (*rx_handler)(struct netif *, void *, uint32_t);
    
    spinlock_irq_t lock;
    struct net_pkt *rx_queue;
    struct net_pkt *tx_queue;
    
    struct netif *next;
};

#define NETIF_FLAG_UP     0x01
#define NETIF_FLAG_BROADCAST 0x02
#define NETIF_FLAG_LOOPBACK 0x04

extern struct netif *netif_list;
extern struct netif *netif_default;

void net_init(void);
struct netif *netif_alloc(const char *name);
int netif_register(struct netif *dev);
struct netif *netif_find_by_name(const char *name);
struct netif *netif_find_by_ip(const uint8_t *ip);

struct net_pkt *pkt_alloc(uint32_t size);
void pkt_free(struct net_pkt *pkt);
void pkt_queue_rx(struct netif *dev, struct net_pkt *pkt);
struct net_pkt *pkt_dequeue_rx(struct netif *dev);
int net_xmit(struct netif *dev, struct net_pkt *pkt);

uint16_t net_checksum(const void *data, uint32_t len);
uint16_t net_ip_checksum(const struct ip_hdr *iph);
uint16_t net_udp_checksum(const struct ip_hdr *iph, const struct udp_hdr *uh, const void *data, uint32_t len);

void arp_init(void);
int arp_request(struct netif *dev, const uint8_t *target_ip);
void arp_reply(struct netif *dev, const struct arp_hdr *arph);
int arp_resolve(struct netif *dev, const uint8_t *ip, uint8_t *mac);
void arp_update(const uint8_t *sender_ip, const uint8_t *sender_mac);
void arp_input(struct netif *dev, void *data, uint32_t len);

void eth_input(struct netif *dev, void *data, uint32_t len);

void ip_init(void);
int ip_output(struct netif *dev, struct net_pkt *pkt, uint8_t proto, const uint8_t *daddr);
void ip_input(struct netif *dev, struct net_pkt *pkt);
uint8_t *ip_local_addr(struct netif *dev);
uint16_t ip_get_id(void);

void icmp_init(void);
int icmp_send_echo_reply(struct netif *dev, const struct ip_hdr *iph, const struct icmp_hdr *icmph);
void icmp_input(struct netif *dev, struct net_pkt *pkt);

void udp_init(void);
struct udp_sock *udp_socket(int domain, int type, int protocol);
int udp_bind(struct udp_sock *sock, const uint8_t *addr, uint16_t port);
int udp_sendto(struct udp_sock *sock, const void *data, uint32_t len, const uint8_t *dst_ip, uint16_t dst_port);
int udp_recvfrom(struct udp_sock *sock, void *buf, uint32_t len, uint8_t *src_ip, uint16_t *src_port);
void udp_close(struct udp_sock *sock);
void udp_input(struct netif *dev, struct net_pkt *pkt);

struct netif *loopback_init(void);

struct udp_sock {
    uint16_t sport;
    uint16_t dport;
    uint8_t  dst_ip[4];
    uint8_t  bound;
    struct netif *dev;

    spinlock_irq_t lock;
    struct net_pkt *rx_queue;   /* head (FIFO) */
    struct net_pkt *rx_tail;    /* tail for O(1) append */
    uint32_t rx_count;          /* queued datagrams */
    uint32_t rx_drops;          /* dropped on full/invalid since creation */
    uint8_t  closed;            /* set by udp_close; wakes blocked recv */
    wait_queue_head_t wait;
};

/* Bound per-socket RX queue: each datagram pins a kernel packet (~2 KiB).
 * 64 caps one socket at ~128 KiB; excess is dropped (rx_drops++). */
#define UDP_MAX_RX_QUEUE 64

#endif