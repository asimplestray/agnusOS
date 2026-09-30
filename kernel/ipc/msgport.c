/*
 * Message ports — AmigaOS-style IPC
 *
 * Kernel-resident FIFO queues of messages.  Messages carry an inline
 * payload, a code and an optional reply_port; the kernel keeps a received
 * message in a pending-reply slot until the receiver replies, at which
 * point the reply payload is queued on the reply port.
 */

#include <msgport.h>
#include <kheap.h>
#include <wait.h>
#include <task.h>
#include <timer.h>
#include <screen.h>
#include <serial.h>
#include <spinlock.h>
#include <string.h>
#include <stddef.h>

typedef struct kmsg {
    struct kmsg *next;
    uint32_t size;
    uint32_t code;
    int32_t  reply_port;
    char     payload[MSG_MAX_PAYLOAD];
} kmsg_t;

typedef struct msg_port {
    struct msg_port *next;
    int32_t id;
    char name[16];
    kmsg_t *head;
    kmsg_t *tail;
    wait_queue_head_t wait;
    spinlock_t lock;
    uint64_t owner;   /* pid of creating task (0 = kernel/no-task) */
} msg_port_t;

typedef struct kreply {
    struct kreply *next;
    kmsg_t *msg;      /* received message kept alive until Reply */
    int32_t reply_port;
    int64_t token;
} kreply_t;

static msg_port_t *port_list = NULL;
static kreply_t   *reply_list = NULL;
static int32_t next_port_id = 1;
static int64_t next_token = 1;
static spinlock_irq_t registry_lock = {{0}, 0};

static msg_port_t *find_port_locked(int32_t id)
{
    for (msg_port_t *p = port_list; p; p = p->next)
        if (p->id == id)
            return p;
    return NULL;
}

int64_t msgport_create(const char *name)
{
    msg_port_t *port = kmalloc(sizeof(msg_port_t));
    if (!port)
        return -1;

    port->next = NULL;
    port->head = NULL;
    port->tail = NULL;
    init_waitqueue_head(&port->wait);
    spinlock_init(&port->lock);
    port->owner = current ? current->pid : 0;

    if (name) {
        int i = 0;
        while (name[i] && name[i] != ':' && i < 15) {
            port->name[i] = name[i];
            i++;
        }
        port->name[i] = '\0';
    } else {
        port->name[0] = '\0';
    }

    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);
    port->id = next_port_id++;
    port->next = port_list;
    port_list = port;
    spin_unlock_irqrestore(&registry_lock, flags);
    return port->id;
}

int64_t msgport_delete(int32_t id)
{
    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);

    msg_port_t **pp = &port_list;
    while (*pp && (*pp)->id != id)
        pp = &(*pp)->next;
    msg_port_t *port = *pp;
    if (!port) {
        spin_unlock_irqrestore(&registry_lock, flags);
        return -1;
    }
    *pp = port->next;

    kmsg_t *km = port->head;
    while (km) {
        kmsg_t *next = km->next;
        kfree(km);
        km = next;
    }

    /* Drop pending replies that target this port */
    kreply_t **rp = &reply_list;
    while (*rp) {
        kreply_t *kr = *rp;
        if (kr->reply_port == id) {
            *rp = kr->next;
            kfree(kr->msg);
            kfree(kr);
        } else {
            rp = &kr->next;
        }
    }

    wake_up(&port->wait);
    spin_unlock_irqrestore(&registry_lock, flags);
    kfree(port);
    return 0;
}

/* Delete every port owned by @t. Called once from task_exit().
 * Loops one port at a time (find under lock, delete without holding it)
 * so we never hold registry_lock across wake_up/kfree. Pending replies
 * targeting a deleted port are purged by msgport_delete(). */
void msgport_task_cleanup(struct task_struct *t)
{
    if (!t)
        return;
    uint64_t pid = t->pid;
    for (;;) {
        int32_t victim = 0;
        unsigned long flags;
        spin_lock_irqsave(&registry_lock, &flags);
        for (msg_port_t *p = port_list; p; p = p->next) {
            if (p->owner == pid) {
                victim = p->id;
                break;
            }
        }
        spin_unlock_irqrestore(&registry_lock, flags);
        if (!victim)
            break;
        msgport_delete(victim);
    }
}

int64_t msgport_put(int32_t id, const msg_t *msg)
{
    if (!msg || msg->size > MSG_MAX_PAYLOAD)
        return -1;

    kmsg_t *km = kmalloc(sizeof(kmsg_t));
    if (!km)
        return -1;

    km->next = NULL;
    km->size = msg->size;
    km->code = msg->code;
    km->reply_port = msg->reply_port;
    for (uint32_t i = 0; i < msg->size; i++)
        km->payload[i] = msg->payload[i];

    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);

    msg_port_t *port = find_port_locked(id);
    if (!port) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(km);
        return -1;
    }

    if (port->tail)
        port->tail->next = km;
    else
        port->head = km;
    port->tail = km;

    wake_up(&port->wait);
    spin_unlock_irqrestore(&registry_lock, flags);
    return 0;
}

int64_t msgport_get(int32_t id, msg_t *msg, int64_t *token)
{
    kreply_t *kr = kmalloc(sizeof(kreply_t));
    if (!kr)
        return -1;

    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);

    msg_port_t *port = find_port_locked(id);
    if (!port) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(kr);
        return -1;
    }

    kmsg_t *km = port->head;
    if (!km) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(kr);
        return MSGPORT_GET_EMPTY;
    }

    port->head = km->next;
    if (!port->head)
        port->tail = NULL;

    if (msg) {
        msg->size = km->size;
        msg->code = km->code;
        msg->reply_port = km->reply_port;
        for (uint32_t i = 0; i < km->size; i++)
            msg->payload[i] = km->payload[i];
    }

    if (km->reply_port >= 0) {
        kr->next = reply_list;
        kr->msg = km;
        kr->reply_port = km->reply_port;
        kr->token = next_token++;
        reply_list = kr;
        if (token)
            *token = kr->token;
        spin_unlock_irqrestore(&registry_lock, flags);
        return MSGPORT_GET_MSG;
    }

    spin_unlock_irqrestore(&registry_lock, flags);
    kfree(km);
    kfree(kr);
    return MSGPORT_GET_MSG;
}

int64_t msgport_wait(int32_t id, int64_t timeout_ms)
{
    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);
    msg_port_t *port = find_port_locked(id);
    spin_unlock_irqrestore(&registry_lock, flags);
    if (!port)
        return -1;

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = timer_get_ticks() + (uint64_t)(timeout_ms / 10);

    DEFINE_WAIT(__w);
    for (;;) {
        spin_lock_irqsave(&registry_lock, &flags);
        if (port->head != NULL) {
            spin_unlock_irqrestore(&registry_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&registry_lock, flags);

        if (timeout_ms > 0 && timer_get_ticks() >= deadline)
            return -1;

        prepare_to_wait(&port->wait, &__w, TASK_STATE_UNINTERRUPTIBLE);
        current->state = TASK_STATE_UNINTERRUPTIBLE;
        schedule();
    }
    finish_wait(&port->wait, &__w);
    return 0;
}

int64_t msgport_reply(int64_t token, const msg_t *msg)
{
    if (!msg || msg->size > MSG_MAX_PAYLOAD)
        return -1;

    kmsg_t *km = kmalloc(sizeof(kmsg_t));
    if (!km)
        return -1;

    km->next = NULL;
    km->size = msg->size;
    km->code = msg->code;
    km->reply_port = -1;
    for (uint32_t i = 0; i < msg->size; i++)
        km->payload[i] = msg->payload[i];

    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);

    kreply_t **rp = &reply_list;
    while (*rp && (*rp)->token != token)
        rp = &(*rp)->next;
    kreply_t *kr = *rp;
    if (!kr) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(km);
        return -1;
    }
    *rp = kr->next;

    msg_port_t *port = find_port_locked(kr->reply_port);
    if (!port) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(km);
        kfree(kr->msg);
        kfree(kr);
        return -1;
    }

    if (port->tail)
        port->tail->next = km;
    else
        port->head = km;
    port->tail = km;

    wake_up(&port->wait);
    spin_unlock_irqrestore(&registry_lock, flags);
    kfree(kr->msg);
    kfree(kr);
    return 0;
}

int msgport_dump(char *buf, int max)
{
    int n = 0;
    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);

    for (msg_port_t *p = port_list; p && n < max - 2; p = p->next) {
        int queued = 0;
        for (kmsg_t *km = p->head; km; km = km->next)
            queued++;
        n += snprintf(buf + n, max - n, "port %d %s: queued=%d\n",
                      p->id, p->name[0] ? p->name : "(anon)", queued);
    }

    int pending = 0;
    for (kreply_t *kr = reply_list; kr; kr = kr->next)
        pending++;
    n += snprintf(buf + n, max - n, "pending_replies=%d\n", pending);

    spin_unlock_irqrestore(&registry_lock, flags);
    return n;
}

static int test_failures = 0;
static void msg_check(int cond, const char *what)
{
    if (!cond) {
        test_failures++;
        serial_print("[MSGPORT-TEST] ");
        serial_print(what);
        serial_print("\n");
        screen_log("FAIL", COLOR_LIGHT_RED, what);
    }
}

void msgport_test(void)
{
    serial_print("[MSGPORT-TEST] starting...\n");
    int64_t a = msgport_create("tstA");
    int64_t b = msgport_create("tstB");
    msg_check(a > 0 && b > 0 && a != b, "MSGPORT-TEST create x2");

    msg_t m;
    m.size = 5;
    m.code = 0x1234;
    m.reply_port = (int32_t)b;
    m.payload[0] = 'h';
    m.payload[1] = 'e';
    m.payload[2] = 'l';
    m.payload[3] = 'l';
    m.payload[4] = 'o';
    msg_check(msgport_put((int32_t)a, &m) == 0, "MSGPORT-TEST put");

    msg_check(msgport_wait((int32_t)a, -1) == 0, "MSGPORT-TEST wait pronto");

    msg_t out;
    int64_t token = 0;
    msg_check(msgport_get((int32_t)a, &out, &token) == MSGPORT_GET_MSG,
              "MSGPORT-TEST get");
    msg_check(out.code == 0x1234 && out.size == 5 && token != 0,
              "MSGPORT-TEST header/token");
    msg_check(out.payload[0] == 'h' && out.payload[4] == 'o',
              "MSGPORT-TEST payload");

    msg_t ack;
    ack.size = 3;
    ack.code = 0xABCD;
    ack.reply_port = -1;
    ack.payload[0] = 'a';
    ack.payload[1] = 'c';
    ack.payload[2] = 'k';
    msg_check(msgport_reply(token, &ack) == 0, "MSGPORT-TEST reply");

    int64_t tok2 = 0;
    msg_check(msgport_get((int32_t)b, &out, &tok2) == MSGPORT_GET_MSG,
              "MSGPORT-TEST get reply");
    msg_check(out.code == 0xABCD && out.payload[0] == 'a',
              "MSGPORT-TEST reply payload");

    msg_check(msgport_get((int32_t)a, &out, &tok2) == MSGPORT_GET_EMPTY,
              "MSGPORT-TEST fila vazia");

    msg_check(msgport_reply(token, &ack) != 0, "MSGPORT-TEST reply unico");

    m.size = MSG_MAX_PAYLOAD + 1;
    msg_check(msgport_put((int32_t)a, &m) != 0, "MSGPORT-TEST payload grande");

    msg_check(msgport_delete((int32_t)a) == 0, "MSGPORT-TEST delete a");
    msg_check(msgport_put((int32_t)a, &m) != 0, "MSGPORT-TEST put apos delete");
    msg_check(msgport_delete((int32_t)b) == 0, "MSGPORT-TEST delete b");

    if (test_failures == 0)
        serial_print("[MSGPORT-TEST] PASS\n");
    screen_log(test_failures == 0 ? "OK" : "FAIL",
               test_failures == 0 ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED,
               test_failures == 0 ? "MSGPORT-TEST PASS" : "MSGPORT-TEST falhou");
}