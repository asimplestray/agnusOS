/* bsdsocket.library — AmigaOS-style BSD socket API implementation
 *
 * Wraps the kernel UDP/ARP stack in AmigaOS bsdsocket.library conventions.
 * SocketBase is per-task (stored in task_struct.socket_base).
 * BSD-like API: Socket(), Bind(), Send(), Recv(), CloseSocket().
 * Tag-based init via SocketBaseTags().
 * Error reporting via IoErr().
 */

#include <bsdsocket.h>
#include <net/net.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <task.h>
#include <wait.h>
#include <spinlock.h>

#define BSDSOCKET_TAG  0x42534453  /* "BSDS" */

/* ------------------------------------------------------------------ */
/* Per-task SocketBase management                                       */
/* ------------------------------------------------------------------ */

static struct SocketBase *sb_alloc(void) {
    struct SocketBase *sb = kmalloc(sizeof(struct SocketBase));
    if (!sb) return NULL;
    memset(sb, 0, sizeof(*sb));
    sb->sb_Version = BSDSOCKET_VERSION;
    sb->sb_Size    = sizeof(struct SocketBase);
    sb->sb_Task    = current;
    sb->sb_Error   = 0;
    sb->sb_Flags   = 0;
    sb->sb_SocketCount = 0;
    for (int i = 0; i < BSDSOCKET_MAX_FDS; i++)
        sb->sb_Socks[i].in_use = 0;
    return sb;
}

static struct SocketBase *get_sb(void) {
    if (!current) return NULL;
    return current->socket_base;
}

static struct SocketBase *ensure_sb(void) {
    struct SocketBase *sb = get_sb();
    if (sb) return sb;
    sb = sb_alloc();
    if (sb) current->socket_base = sb;
    return sb;
}

static void set_err(int err) {
    struct SocketBase *sb = get_sb();
    if (sb) sb->sb_Error = err;
}

/* ------------------------------------------------------------------ */
/* SocketBaseTags — AmigaOS-style tag-based initialization              */
/*                                                                     */
/* Usage:                                                              */
/*   struct TagItem tags[] = {                                          */
/*       { SBT_MaxSockets, 16 },                                       */
/*       { TAG_DONE, 0 }                                               */
/*   };                                                                */
/*   SocketBaseTags(tags);                                              */
/* Returns 0 on success.                                               */
/* ------------------------------------------------------------------ */

int SocketBaseTags(struct TagItem *taglist) {
    struct SocketBase *sb = ensure_sb();
    if (!sb) { set_err(AOS_ERR_NO_MEMORY); return -1; }

    if (!taglist) return 0;

    for (struct TagItem *t = taglist; t->ti_Tag != TAG_DONE; t++) {
        switch (t->ti_Tag) {
        case SBT_MaxSockets:
            /* hint only — we use fixed BSDSOCKET_MAX_FDS */
            break;
        case SBT_Task:
            sb->sb_Task = (task_struct_t *)(uintptr_t)t->ti_Data;
            break;
        default:
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket — create a new socket                                         */
/*                                                                     */
/* Returns fd (>=0) or -1 on error. IoErr() gives the AOS_ERR_ code.  */
/* ------------------------------------------------------------------ */

int Socket(int domain, int type, int protocol) {
    struct SocketBase *sb = ensure_sb();
    if (!sb) { set_err(AOS_ERR_NO_MEMORY); return -1; }

    if (domain != AF_INET) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    /* Find free slot */
    int fd = -1;
    for (int i = 0; i < BSDSOCKET_MAX_FDS; i++) {
        if (!sb->sb_Socks[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        set_err(AOS_ERR_NO_MEMORY);
        return -1;
    }

    /* Create underlying UDP socket (TCP not yet supported) */
    struct udp_sock *usock = udp_socket(domain, type, protocol);
    if (!usock) {
        set_err(AOS_ERR_NO_MEMORY);
        return -1;
    }

    struct bsd_sock *bs = &sb->sb_Socks[fd];
    memset(bs, 0, sizeof(*bs));
    bs->in_use    = 1;
    bs->domain    = domain;
    bs->type      = type;
    bs->protocol  = protocol;
    bs->bound     = 0;
    bs->connected = 0;
    bs->proto_sock = usock;
    bs->sb        = sb;
    usock->dev    = netif_default;

    sb->sb_SocketCount++;

    serial_print("bsdsocket: Socket()\n");

    return fd;
}

/* ------------------------------------------------------------------ */
/* Bind — bind socket to a local address                                */
/* ------------------------------------------------------------------ */

int Bind(int sockfd, const struct sockaddr *name, int namelen) {
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !name || namelen < (int)sizeof(struct sockaddr_in)) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)name;
    if (sin->sin_family != AF_INET && sin->sin_family != 0) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
    int rc = udp_bind(usock, (const uint8_t *)&sin->sin_addr, sin->sin_port);

    if (rc == 0) {
        bs->bound = 1;
        bs->local_port = sin->sin_port;
        memcpy(bs->local_addr, &sin->sin_addr, 4);
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* Send — send data (uses remote address if bound, else error)          */
/* ------------------------------------------------------------------ */

int Send(int sockfd, const void *buf, int len, int flags) {
    (void)flags;
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !buf || len <= 0) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    if (!bs->connected) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
    int rc = udp_sendto(usock, buf, len, bs->remote_addr, bs->remote_port);
    return rc;
}

/* ------------------------------------------------------------------ */
/* SendTo — send data to a specific address (UDP)                       */
/* ------------------------------------------------------------------ */

int SendTo(int sockfd, const void *buf, int len, int flags,
           const struct sockaddr *to, int tolen) {
    (void)flags;
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !buf || len <= 0) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    if (!to || tolen < (int)sizeof(struct sockaddr_in)) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)to;
    struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;

    /* Auto-bind if not bound yet */
    if (!bs->bound) {
        udp_bind(usock, NULL, 0);
        bs->bound = 1;
    }

    int rc = udp_sendto(usock, buf, len,
                        (const uint8_t *)&sin->sin_addr, sin->sin_port);

    return rc;
}

/* ------------------------------------------------------------------ */
/* Recv — receive data (blocking)                                       */
/* ------------------------------------------------------------------ */

int Recv(int sockfd, void *buf, int len, int flags) {
    (void)flags;
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !buf || len <= 0) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
    uint8_t src_ip[4];
    uint16_t src_port;
    int rc = udp_recvfrom(usock, buf, len, src_ip, &src_port);

    /* Auto-connect on first receive (AmigaOS convention for connected UDP) */
    if (rc > 0 && !bs->connected) {
        bs->connected = 1;
        memcpy(bs->remote_addr, src_ip, 4);
        bs->remote_port = src_port;
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* RecvFrom — receive data + get sender address                         */
/* ------------------------------------------------------------------ */

int RecvFrom(int sockfd, void *buf, int len, int flags,
             struct sockaddr *from, int *fromlen) {
    (void)flags;
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !buf || len <= 0) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
    uint8_t src_ip[4];
    uint16_t src_port;
    int rc = udp_recvfrom(usock, buf, len, src_ip, &src_port);

    if (rc > 0 && from && fromlen && *fromlen >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)from;
        sin->sin_family = AF_INET;
        sin->sin_port   = src_port;
        memcpy(&sin->sin_addr, src_ip, 4);
        memset(sin->sin_zero, 0, 8);
        *fromlen = sizeof(struct sockaddr_in);
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* CloseSocket — close a socket                                         */
/* ------------------------------------------------------------------ */

int CloseSocket(int sockfd) {
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
    if (usock && usock->bound)
        udp_close(usock);

    bs->in_use = 0;
    bs->proto_sock = NULL;
    sb->sb_SocketCount--;

    serial_print("bsdsocket: CloseSocket()\n");

    return 0;
}

/* ------------------------------------------------------------------ */
/* GetSocketAddr — get local address of a bound socket                  */
/* ------------------------------------------------------------------ */

int GetSocketAddr(int sockfd, struct sockaddr *name, int *namelen) {
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !name || !namelen ||
        *namelen < (int)sizeof(struct sockaddr_in)) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    struct sockaddr_in *sin = (struct sockaddr_in *)name;
    sin->sin_family = AF_INET;
    sin->sin_port   = bs->local_port;
    memcpy(&sin->sin_addr, bs->local_addr, 4);
    memset(sin->sin_zero, 0, 8);
    *namelen = sizeof(struct sockaddr_in);

    return 0;
}

/* ------------------------------------------------------------------ */
/* SetSockOpt / GetSockOpt — socket options                             */
/* ------------------------------------------------------------------ */

int SetSockOpt(int sockfd, int level, int optname, const void *optval, int optlen) {
    (void)level; (void)optval; (void)optlen;
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    switch (optname) {
    case SO_BROADCAST:
        /* accept but no real effect yet */
        return 0;
    case SO_REUSEADDR:
        return 0;
    case SO_KEEPALIVE:
        return 0;
    default:
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
}

int GetSockOpt(int sockfd, int level, int optname, void *optval, int *optlen) {
    (void)level;
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use || !optval || !optlen) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    switch (optname) {
    case SO_BROADCAST:
    case SO_REUSEADDR:
    case SO_KEEPALIVE: {
        int val = 1;
        if (*optlen >= (int)sizeof(int)) {
            memcpy(optval, &val, sizeof(int));
            *optlen = sizeof(int);
        }
        return 0;
    }
    default:
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Select — check I/O readiness (simplified, polling)                   */
/*                                                                     */
/* For each fd in readfds: check if rx_queue has data.                  */
/* Returns number of ready fds, 0 on timeout, -1 on error.             */
/* ------------------------------------------------------------------ */

int Select(int width, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    (void)writefds; (void)exceptfds; (void)timeout;
    struct SocketBase *sb = get_sb();
    if (!sb || width <= 0 || width > FD_SETSIZE) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    int ready = 0;
    fd_set result_read;
    FD_ZERO(&result_read);

    for (int i = 0; i < width; i++) {
        if (readfds && FD_ISSET(i, readfds)) {
            struct bsd_sock *bs = &sb->sb_Socks[i];
            if (bs->in_use && bs->proto_sock) {
                struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
                if (usock->rx_queue) {
                    FD_SET(i, &result_read);
                    ready++;
                }
            }
        }
    }

    if (readfds) *readfds = result_read;
    return ready;
}

/* ------------------------------------------------------------------ */
/* IoErr / ClearIoErr                                                   */
/* ------------------------------------------------------------------ */

int IoErr(void) {
    struct SocketBase *sb = get_sb();
    return sb ? sb->sb_Error : AOS_ERR_BAD_ARGUMENT;
}

void ClearIoErr(void) {
    struct SocketBase *sb = get_sb();
    if (sb) sb->sb_Error = 0;
}

/* ------------------------------------------------------------------ */
/* SocketIOCtl — query socket I/O status                                 */
/* ------------------------------------------------------------------ */

int SocketIOCtl(int sockfd, int request, void *arg) {
    struct SocketBase *sb = get_sb();
    if (!sb || sockfd < 0 || sockfd >= BSDSOCKET_MAX_FDS) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
    struct bsd_sock *bs = &sb->sb_Socks[sockfd];
    if (!bs->in_use) {
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }

    switch (request) {
    case FIONREAD: {
        /* Return number of bytes waiting in rx_queue */
        int nbytes = 0;
        if (bs->proto_sock) {
            struct udp_sock *usock = (struct udp_sock *)bs->proto_sock;
            struct net_pkt *p = usock->rx_queue;
            while (p) {
                /* rough estimate: IP header + UDP header + payload */
                nbytes += p->len;
                p = p->next;
            }
        }
        if (arg) *(int *)arg = nbytes;
        return 0;
    }
    case FIONBIO:
        /* accept but no real non-blocking support yet */
        return 0;
    default:
        set_err(AOS_ERR_BAD_ARGUMENT);
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* bsdsocket_task_exit — cleanup on task termination                     */
/* ------------------------------------------------------------------ */

void bsdsocket_task_exit(struct SocketBase *sb) {
    if (!sb) return;

    for (int i = 0; i < BSDSOCKET_MAX_FDS; i++) {
        if (sb->sb_Socks[i].in_use) {
            struct udp_sock *usock = (struct udp_sock *)sb->sb_Socks[i].proto_sock;
            if (usock && usock->bound)
                udp_close(usock);
            sb->sb_Socks[i].in_use = 0;
        }
    }

    kfree(sb);
}

/* ------------------------------------------------------------------ */
/* bsdsocket_init — called once at boot                                 */
/* ------------------------------------------------------------------ */

void bsdsocket_init(void) {
    serial_print("ApolloOS: bsdsocket.library v1.0 initialized\n");
}
