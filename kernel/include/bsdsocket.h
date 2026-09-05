/* bsdsocket.library — AmigaOS-style BSD socket API
 *
 * Wraps the kernel UDP stack in an AmigaOS library interface.
 * Follows the AmigaOS bsdsocket.library conventions:
 *   - SocketBase per-task library pointer
 *   - BSD-like function names (Socket, Bind, Send, Recv, CloseSocket)
 *   - Tag-based initialization (SocketBaseTags)
 *   - IoErr() for error reporting
 *   - TagItem parameter passing
 *
 * Versão: 1.0 — bsdsocket-1.0
 */

#ifndef BSDSOCKET_H
#define BSDSOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <syscall.h>    /* sockaddr, sockaddr_in, AF_INET, SOCK_* */

/* ------------------------------------------------------------------ */
/* Library version                                                      */
/* ------------------------------------------------------------------ */

#define BSDSOCKET_VERSION  1
#define BSDSOCKET_REVISION 0

/* ------------------------------------------------------------------ */
/* TagItem — AmigaOS-style parameter passing                            */
/* ------------------------------------------------------------------ */

struct TagItem {
    uint32_t ti_Tag;    /* tag identifier */
    uint32_t ti_Data;   /* value or pointer */
};

#define TAG_DONE    0   /* end of tag list */
#define TAG_IGNORE  1   /* skip this tag */

/* SocketBaseTags tag IDs */
#define SBT_BindAddr     0x0001   /* IP address to bind (uint32_t) */
#define SBT_BindPort     0x0002   /* port number (uint16_t) */
#define SBT_MaxSockets   0x0003   /* max sockets (uint32_t, hint) */
#define SBT_Task         0x0004   /* task pointer (for kernel use) */

/* ------------------------------------------------------------------ */
/* Protocol numbers                                                     */
/* ------------------------------------------------------------------ */

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP    1
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP     6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP     17
#endif

/* ------------------------------------------------------------------ */
/* Socket flags                                                         */
/* ------------------------------------------------------------------ */

#define MSG_DONTWAIT    0x01    /* non-blocking I/O */
#define MSG_PEEK        0x02    /* peek at incoming data */
#define MSG_WAITALL     0x04    /* wait for full buffer */

/* ------------------------------------------------------------------ */
/* SetSockOpt levels & options                                          */
/* ------------------------------------------------------------------ */

#define SOL_SOCKET      0

#define SO_REUSEADDR    0x0001
#define SO_KEEPALIVE    0x0002
#define SO_BROADCAST    0x0004
#define SO_LINGER       0x0008
#define SO_SNDBUF       0x0010
#define SO_RCVBUF       0x0020

/* ------------------------------------------------------------------ */
/* select() fd_set — simple bitmask (max 32 sockets)                    */
/* ------------------------------------------------------------------ */

#define FD_SETSIZE  32

typedef struct {
    uint32_t bits;
} fd_set;

static inline void FD_ZERO(fd_set *set) { set->bits = 0; }
static inline void FD_SET(int fd, fd_set *set) { if (fd >= 0 && fd < FD_SETSIZE) set->bits |= (1U << fd); }
static inline void FD_CLR(int fd, fd_set *set) { if (fd >= 0 && fd < FD_SETSIZE) set->bits &= ~(1U << fd); }
static inline int  FD_ISSET(int fd, const fd_set *set) { return (fd >= 0 && fd < FD_SETSIZE) && (set->bits & (1U << fd)); }

/* ------------------------------------------------------------------ */
/* ioctl request codes (AmigaOS DoIO style)                             */
/* ------------------------------------------------------------------ */

#define FIONBIO         0x5421  /* set non-blocking */
#define FIONREAD        0x541B  /* bytes available to read */
#define SIOCGIFADDR     0x8915  /* get interface address */

/* ------------------------------------------------------------------ */
/* InAddr — shorthand for IP address                                   */
/* ------------------------------------------------------------------ */

#ifndef INADDR_ANY
#define INADDR_ANY      0
#endif
#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST 0xFFFFFFFF
#endif

typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

/* ------------------------------------------------------------------ */
/* Timeval for Select                                                   */
/* ------------------------------------------------------------------ */

struct timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

/* ------------------------------------------------------------------ */
/* SocketBase — per-task library state                                  */
/* ------------------------------------------------------------------ */

#define BSDSOCKET_MAX_FDS  32

struct SocketBase;

/* Socket descriptor entry — internal */
struct bsd_sock {
    int                  in_use;
    int                  domain;      /* AF_INET */
    int                  type;        /* SOCK_DGRAM / SOCK_STREAM */
    int                  protocol;    /* IPPROTO_UDP etc */
    uint16_t             local_port;
    uint8_t              local_addr[4];
    uint16_t             remote_port;
    uint8_t              remote_addr[4];
    int                  bound;
    int                  connected;
    void                *proto_sock;   /* → struct udp_sock */
    struct SocketBase   *sb;          /* back-pointer */
};

struct SocketBase {
    uint32_t             sb_Version;
    uint32_t             sb_Size;
    task_struct_t      *sb_Task;      /* owning task */
    int                  sb_Error;     /* last error (IoErr) */
    struct bsd_sock      sb_Socks[BSDSOCKET_MAX_FDS];
    uint32_t             sb_Flags;
    uint32_t             sb_SocketCount;
};

/* ------------------------------------------------------------------ */
/* Prototypes — AmigaOS bsdsocket.library API                           */
/*                                                                     */
/* These are the kernel-side implementations. Userland calls them      */
/* via AOS_Socket, AOS_Bind, etc. traps.                                */
/* ------------------------------------------------------------------ */

int  SocketBaseTags(struct TagItem *taglist);
int  Socket(int domain, int type, int protocol);
int  Bind(int sockfd, const struct sockaddr *name, int namelen);
int  Send(int sockfd, const void *buf, int len, int flags);
int  SendTo(int sockfd, const void *buf, int len, int flags,
            const struct sockaddr *to, int tolen);
int  Recv(int sockfd, void *buf, int len, int flags);
int  RecvFrom(int sockfd, void *buf, int len, int flags,
              struct sockaddr *from, int *fromlen);
int  CloseSocket(int sockfd);
int  GetSocketAddr(int sockfd, struct sockaddr *name, int *namelen);
int  SetSockOpt(int sockfd, int level, int optname, const void *optval, int optlen);
int  GetSockOpt(int sockfd, int level, int optname, void *optval, int *optlen);
int  Select(int width, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            struct timeval *timeout);
int  IoErr(void);
void ClearIoErr(void);
int  SocketIOCtl(int sockfd, int request, void *arg);

/* ------------------------------------------------------------------ */
/* Internal — called by kernel on task exit                             */
/* ------------------------------------------------------------------ */

void bsdsocket_task_exit(struct SocketBase *sb);
void bsdsocket_init(void);

#endif /* BSDSOCKET_H */
