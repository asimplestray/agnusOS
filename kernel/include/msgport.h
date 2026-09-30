#ifndef MSGPORT_H
#define MSGPORT_H

#include <stdint.h>

/* AmigaOS-style message ports.
 *
 * A port is a named FIFO queue of messages.  The sender hands a message
 * to kernel (msgt port_put), optionally naming a reply_port; the receiver
 * wait()s until a message arrives, get()s it — receiving an opaque token
 * when a reply is expected — and later reply()s on the token, which
 * forwards the reply payload to the sender's port.  The kernel keeps the
 * original message alive until Reply, so reuses the Amiga 'reply exactly
 * once' contract.
 */

#define MSG_MAX_PAYLOAD 128

typedef struct msg_t {
    uint32_t size;        /* payload bytes (0..MSG_MAX_PAYLOAD) */
    uint32_t code;        /* user-defined message code */
    int32_t  reply_port;  /* port expecting the reply, or -1 */
    char     payload[MSG_MAX_PAYLOAD];
} msg_t;

#define MSGPORT_GET_EMPTY  0
#define MSGPORT_GET_MSG    1

/* Create a named (or anonymous, if name == NULL) port. Returns new port id
 * or -1 on failure. */
int64_t msgport_create(const char *name);

/* Delete a port, purging its queue and pending replies. Returns 0. */
int64_t msgport_delete(int32_t id);

/* Send msg to port id (payload is copied into kernel memory). Returns 0. */
int64_t msgport_put(int32_t id, const msg_t *msg);

/* Receive from port id.  On success copies the message out and returns
 * MSGPORT_GET_MSG; *token is set to a reply token when the message had a
 * reply_port.  Returns MSGPORT_GET_EMPTY when the queue is empty. */
int64_t msgport_get(int32_t id, msg_t *msg, int64_t *token);

/* Block until the port has at least one message, or timeout_ms elapses
 * (timeout_ms <= 0 waits forever). Returns 0 on message, -1 on timeout. */
int64_t msgport_wait(int32_t id, int64_t timeout_ms);

/* Deliver msg as the reply for token, waking the reply port's queue.
 * Exactly one reply per token. */
int64_t msgport_reply(int64_t token, const msg_t *msg);

/* Dump ports + pending replies into buf (used by /proc/ports). */
int  msgport_dump(char *buf, int max);

/* Delete all ports owned by @t (called from task_exit). */
struct task_struct;
void msgport_task_cleanup(struct task_struct *t);

/* Boot-time selftest. */
void msgport_test(void);

#endif