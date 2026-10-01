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
    /* Lifetime: +1 for list membership, +1 per pinned user (a waiter
     * across schedule()). Delete unlinks, marks dead, wakes waiters and
     * drops the list ref; the object is freed when the last pin drops,
     * so no waiter ever resumes on freed memory. All under registry_lock. */
    int refcount;
    int dead;
} msg_port_t;

typedef struct kreply {
    struct kreply *next;
    kmsg_t *msg;      /* received message kept alive until Reply */
    int32_t reply_port;
    int64_t token;
    uint64_t owner;   /* pid that did Get (only it may Reply) */
} kreply_t;

/* Quotas live in <msgport.h> (MSGPORT_MAX_*). Put() beyond MAX_QUEUE
 * fails with -1 (backpressure) instead of growing the FIFO unbounded. */

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

/* registry_lock must be held. Finds a listed (non-deleted) port and pins
 * it. The caller must drop the pin with port_release_locked() — which
 * may free. */
static msg_port_t *port_hold_locked(int32_t id)
{
    msg_port_t *p = find_port_locked(id);
    if (p)
        p->refcount++;
    return p;
}

/* registry_lock must be held. Frees the port (queue already purged by the
 * deleter) once list membership and all pins are gone. */
static void port_release_locked(msg_port_t *port)
{
    if (!port)
        return;
    port->refcount--;
    if (port->refcount == 0)
        kfree(port);
}

static uint64_t caller_pid(void) {
    return current ? current->pid : 0;
}

static int ports_owned_by(uint64_t pid) {
    int n = 0;
    for (msg_port_t *p = port_list; p; p = p->next)
        if (p->owner == pid)
            n++;
    return n;
}

static int replies_owned_by(uint64_t pid) {
    int n = 0;
    for (kreply_t *kr = reply_list; kr; kr = kr->next)
        if (kr->owner == pid)
            n++;
    return n;
}

static int queue_len_locked(msg_port_t *port) {
    int n = 0;
    for (kmsg_t *km = port->head; km; km = km->next)
        n++;
    return n;
}

int64_t msgport_create(const char *name)
{
    uint64_t pid = caller_pid();
    unsigned long qflags;
    spin_lock_irqsave(&registry_lock, &qflags);
    int owned = ports_owned_by(pid);
    spin_unlock_irqrestore(&registry_lock, qflags);
    if (pid != 0 && owned >= MSGPORT_MAX_PER_TASK)
        return -1;

    msg_port_t *port = kmalloc(sizeof(msg_port_t));
    if (!port)
        return -1;

    port->next = NULL;
    port->head = NULL;
    port->tail = NULL;
    init_waitqueue_head(&port->wait);
    spinlock_init(&port->lock);
    port->owner = current ? current->pid : 0;
    port->refcount = 1;   /* list membership */
    port->dead = 0;

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
    /* Guard against int32 wraparound after ~2B creates: never hand out
     * id <= 0 (0 means "no port" for callers). Best-effort reuse scan. */
    if (next_port_id <= 0) {
        int32_t cand = 1;
        while (cand > 0 && find_port_locked(cand))
            cand++;
        if (cand <= 0) {
            spin_unlock_irqrestore(&registry_lock, flags);
            kfree(port);
            return -1;
        }
        next_port_id = cand;
    }
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
    /* Only the owner (or pid 0 kernel context) may delete. Senders with
     * just the id cannot tear down someone else's queue. */
    uint64_t pid = caller_pid();
    if (pid != 0 && port->owner != 0 && port->owner != pid) {
        spin_unlock_irqrestore(&registry_lock, flags);
        return -1;
    }
    *pp = port->next;

    kmsg_t *km = port->head;
    port->head = NULL;
    port->tail = NULL;
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

    /* Unlinked ports stay alive while pinned waiters exist: mark dead so
     * woken waiters re-lookup by id, fail, and return -1 instead of
     * touching a stale queue. */
    port->dead = 1;
    wake_up(&port->wait);
    port_release_locked(port);
    spin_unlock_irqrestore(&registry_lock, flags);
    return 0;
}

/* Delete every port owned by @t. Called once from task_exit().
 * Runs as the exiting task, so msgport_delete's owner check passes for
 * its own ports. Delete unlinks + purges + wakes (freed only when the
 * last waiter pin drops), hence the one-at-a-time loop. */
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
    /* Final sweep: tokens owned by pid whose port outlived them
     * (e.g. reply_port belongs to another task that is still alive). */
    unsigned long flags;
    spin_lock_irqsave(&registry_lock, &flags);
    kreply_t **rp = &reply_list;
    while (*rp) {
        kreply_t *kr = *rp;
        if (kr->owner == pid) {
            *rp = kr->next;
            kfree(kr->msg);
            kfree(kr);
        } else {
            rp = &kr->next;
        }
    }
    spin_unlock_irqrestore(&registry_lock, flags);
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
    /* Backpressure: never queue without bound (each kmsg is kernel heap). */
    if (queue_len_locked(port) >= MSGPORT_MAX_QUEUE) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(km);
        return -1;
    }
    /* Validate reply target early: a bogus reply_port would otherwise
     * surface only at Reply time, after the sender already succeeded. */
    if (msg->reply_port >= 0 && !find_port_locked(msg->reply_port)) {
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
    /* Only the owner may drain its queue (Put stays open to any sender). */
    uint64_t pid = caller_pid();
    if (pid != 0 && port->owner != 0 && port->owner != pid) {
        spin_unlock_irqrestore(&registry_lock, flags);
        kfree(kr);
        return -1;
    }
    /* Bound pending replies per getter: each pending reply pins one kmsg. */
    if (pid != 0 && replies_owned_by(pid) >= MSGPORT_MAX_REPLIES_PER_TASK) {
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
        /* Token wraparound: never hand out 0 (means "no reply expected"). */
        if (next_token <= 0)
            next_token = 1;
        kr->owner = caller_pid();
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
    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = timer_get_ticks() + (uint64_t)(timeout_ms / 10);

    DEFINE_WAIT(__w);
    for (;;) {
        msg_port_t *port;
        unsigned long flags;
        spin_lock_irqsave(&registry_lock, &flags);
        port = port_hold_locked(id);
        if (!port) {
            /* Deleted while (or before) waiting: -1, never a stale queue. */
            spin_unlock_irqrestore(&registry_lock, flags);
            return -1;
        }
        uint64_t pid = caller_pid();
        if (pid != 0 && port->owner != 0 && port->owner != pid) {
            port_release_locked(port);
            spin_unlock_irqrestore(&registry_lock, flags);
            return -1;
        }
        if (port->head != NULL) {
            port_release_locked(port);
            spin_unlock_irqrestore(&registry_lock, flags);
            break;
        }
        /* Already pinned by port_hold_locked: the pin survives the sleep,
         * so delete can unlink + free only after our last finish_wait +
         * port_release_locked. Re-lookup by id every lap: the pointer is
         * never trusted across schedule(). */
        spin_unlock_irqrestore(&registry_lock, flags);

        if (timeout_ms > 0 && timer_get_ticks() >= deadline) {
            spin_lock_irqsave(&registry_lock, &flags);
            port_release_locked(port);
            spin_unlock_irqrestore(&registry_lock, flags);
            return -1;
        }

        prepare_to_wait(&port->wait, &__w, TASK_STATE_UNINTERRUPTIBLE);
        /* Lost-wakeup window (unlock above → prepare): re-check under
         * lock; a message or a delete in between short-circuits the
         * sleep instead of blocking until the next event. */
        spin_lock_irqsave(&registry_lock, &flags);
        /* dead covers id reuse: a new port may already own this id while
         * our pinned object is unlinked — never sleep on a dead queue. */
        int evicted = (port->dead || find_port_locked(id) != port);
        int arrived = (!evicted && port->head != NULL);
        spin_unlock_irqrestore(&registry_lock, flags);
        if (evicted || arrived) {
            finish_wait(&port->wait, &__w);
            spin_lock_irqsave(&registry_lock, &flags);
            port_release_locked(port);
            spin_unlock_irqrestore(&registry_lock, flags);
            if (arrived)
                break;
            continue;   /* re-lookup reports the deletion as -1 */
        }
        current->state = TASK_STATE_UNINTERRUPTIBLE;
        schedule();
        /* Woken: drop the pin before re-looking up, so a concurrent
         * delete can complete while we loop back to find_port_locked. */
        spin_lock_irqsave(&registry_lock, &flags);
        finish_wait(&port->wait, &__w);
        port_release_locked(port);
        spin_unlock_irqrestore(&registry_lock, flags);
    }
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
    /* Single-reply token is bound to the task that did Get. Any other
     * task presenting the token gets -1 (no cross-task reply spoofing). */
    uint64_t pid = caller_pid();
    if (pid != 0 && kr->owner != 0 && kr->owner != pid) {
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
static void msgport_race_test(void);
static void msgport_ordering_test(void);

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

    msgport_race_test();
    msgport_ordering_test();

    if (test_failures == 0)
        serial_print("[MSGPORT-TEST] PASS\n");
    screen_log(test_failures == 0 ? "OK" : "FAIL",
               test_failures == 0 ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED,
               test_failures == 0 ? "MSGPORT-TEST PASS" : "MSGPORT-TEST falhou");
}

/* Cross-task wait/wake/reply sob owner-auth: W cria P1 (dono=W), publica
 * o id e bloqueia em wait; main (dono de P2) envia com reply_port=P2; W
 * acorda, recebe + responde; main confere o ack em P2. Polling com
 * schedule() e teto de iterações — sem dependência do timer. Ao sair, W
 * morre e o cleanup apaga P1 (verificado pelo main). */
static volatile int32_t race_p1 = 0;
static volatile int race_done = 0;
static volatile int64_t race_wrc = -99;

static void msgport_race_waiter(void) {
    int64_t p1 = msgport_create("tstR1");
    if (p1 <= 0) {
        race_done = 1;
        return;
    }
    race_p1 = (int32_t)p1;
    int64_t wrc = msgport_wait((int32_t)p1, 0);
    race_wrc = wrc;
    if (wrc != 0) {
        race_done = 1;
        return;
    }
    msg_t out;
    int64_t tok = 0;
    if (msgport_get((int32_t)p1, &out, &tok) != MSGPORT_GET_MSG) {
        race_done = 1;
        return;
    }
    msg_t ack;
    ack.size = 2;
    ack.code = 0x7777;
    ack.reply_port = -1;
    ack.payload[0] = 'o';
    ack.payload[1] = 'k';
    msgport_reply(tok, &ack);
    race_done = 1;
}

static int race_poll(volatile int *flag, int want, unsigned iters) {
    while (iters-- && *flag != want)
        schedule();
    return *flag == want;
}

static void msgport_race_test(void) {
    serial_print("[MSGPORT-TEST] race: cross-task wait/wake/reply...\n");
    race_p1 = 0;
    race_done = 0;
    race_wrc = -99;

    int64_t p2 = msgport_create("tstR2");
    msg_check(p2 > 0, "MSGPORT-TEST race create p2");
    if (p2 <= 0)
        return;
    task_struct_t *w = task_create(msgport_race_waiter, 0);
    msg_check(w != NULL, "MSGPORT-TEST race spawn waiter");
    if (!w) {
        msgport_delete((int32_t)p2);
        return;
    }

    /* Aguarda o waiter publicar P1 (teto: sem hang no boot em falha). */
    unsigned pub_iters = 200000;
    while (pub_iters-- && race_p1 == 0)
        schedule();
    msg_check(race_p1 > 0, "MSGPORT-TEST race waiter publicou P1");
    if (race_p1 <= 0) {
        msgport_delete((int32_t)p2);
        return;
    }
    int32_t p1 = race_p1;
    /* Dá tempo do waiter bloquear (se a msg chegar antes, o wait retorna
     * direto — ambos os caminhos valem). */
    for (unsigned i = 0; i < 50000; i++)
        schedule();

    msg_t m;
    m.size = 4;
    m.code = 0x5555;
    m.reply_port = (int32_t)p2;
    m.payload[0] = 'p';
    m.payload[1] = 'i';
    m.payload[2] = 'n';
    m.payload[3] = 'g';
    msg_check(msgport_put(p1, &m) == 0, "MSGPORT-TEST race put cross-task");
    msg_check(race_poll(&race_done, 1, 200000), "MSGPORT-TEST race waiter acordou");
    msg_check(race_wrc == 0, "MSGPORT-TEST race wait rc 0");

    msg_t out;
    int64_t tok = 0;
    msg_check(msgport_get((int32_t)p2, &out, &tok) == MSGPORT_GET_MSG,
              "MSGPORT-TEST race get reply");
    msg_check(out.code == 0x7777 && out.size == 2 &&
              out.payload[0] == 'o' && out.payload[1] == 'k',
              "MSGPORT-TEST race reply payload");

    msg_check(msgport_delete((int32_t)p2) == 0, "MSGPORT-TEST race delete p2");
    /* Waiter saiu → cleanup apagou P1: get devolve erro sem UAF (só
     * chamadas sem bloqueio aqui — wait sem timeout travaria o boot). */
    for (unsigned i = 0; i < 50000; i++)
        schedule();
    msg_check(msgport_get(p1, &out, &tok) != MSGPORT_GET_MSG,
              "MSGPORT-TEST race P1 sumiu com o dono");
}

/* Ordenação sem concorrência: wait/delete em ids mortos e fila cheia. */
static void msgport_ordering_test(void) {
    msg_t m;
    m.size = 1;
    m.code = 1;
    m.reply_port = -1;
    m.payload[0] = 'x';

    msg_check(msgport_wait(0x7FFFFFFF, 0) == -1, "MSGPORT-TEST wait id morto");
    msg_check(msgport_delete(0x7FFFFFFF) == -1, "MSGPORT-TEST delete id morto");
    msg_check(msgport_delete(0x7FFFFFFF) == -1, "MSGPORT-TEST delete 2x");

    int64_t q = msgport_create("tstQ");
    msg_check(q > 0, "MSGPORT-TEST queue create");
    if (q <= 0)
        return;
    int i;
    for (i = 0; i < MSGPORT_MAX_QUEUE; i++) {
        if (msgport_put((int32_t)q, &m) != 0)
            break;
    }
    msg_check(i == MSGPORT_MAX_QUEUE, "MSGPORT-TEST queue enche 32");
    msg_check(msgport_put((int32_t)q, &m) != 0, "MSGPORT-TEST queue backpressure");
    msg_check(msgport_delete((int32_t)q) == 0, "MSGPORT-TEST queue delete");
}