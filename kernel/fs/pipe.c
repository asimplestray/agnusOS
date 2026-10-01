#include <vfs.h>
#include <kheap.h>
#include <wait.h>
#include <screen.h>
#include <spinlock.h>
#include <task.h>
#include <serial.h>
#include <timer.h>

#define PIPE_BUF_SIZE 4096

typedef struct pipe_data {
    char buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t size;
    int readers;
    int writers;
    wait_queue_head_t read_wait;
    wait_queue_head_t write_wait;
    spinlock_irq_t lock;
    vfs_node_t *read_node;
    vfs_node_t *write_node;
    /* Lifetime: +1 por ponta viva (read+write = 2) e +1 por dorminhoco
     * com pin através do sleep. O último close marca dead, acorda,
     * libera os nodes e solta as 2 refs das pontas; o objeto só é
     * liberado quando o último pin cair. Nenhum waiter retoma em
     * memória liberada. Tudo sob lock. */
    int refcount;
    int dead;
} pipe_data_t;

static uint32_t pipe_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)offset;
    pipe_data_t *pipe = (pipe_data_t *)node->ptr;
    if (!pipe) return 0;

    unsigned long flags;
    spin_lock_irqsave(&pipe->lock, &flags);
    if (pipe->dead) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        return 0;
    }
    pipe->refcount++;   /* pin através dos sleeps abaixo */
    spin_unlock_irqrestore(&pipe->lock, flags);

    while (1) {
        spin_lock_irqsave(&pipe->lock, &flags);
        if (pipe->dead || (pipe->size == 0 && pipe->writers == 0)) {
            int free_pipe = 0;
            pipe->refcount--;
            if (pipe->refcount == 0)
                free_pipe = 1;
            spin_unlock_irqrestore(&pipe->lock, flags);
            if (free_pipe)
                kfree(pipe);
            return 0;   /* EOF: ponta oposta fechou ou pipe morto */
        }
        if (pipe->size > 0)
            break;      /* com lock preso: cai no consumo abaixo */
        spin_unlock_irqrestore(&pipe->lock, flags);
        wait_event_interruptible(pipe->read_wait, pipe->size > 0 || pipe->writers == 0);
    }

    uint32_t to_read = size < pipe->size ? size : pipe->size;
    uint32_t first_chunk = PIPE_BUF_SIZE - pipe->read_pos;
    if (to_read <= first_chunk) {
        for (uint32_t i = 0; i < to_read; i++) {
            buf[i] = pipe->buffer[pipe->read_pos + i];
        }
        pipe->read_pos = (pipe->read_pos + to_read) % PIPE_BUF_SIZE;
    } else {
        for (uint32_t i = 0; i < first_chunk; i++) {
            buf[i] = pipe->buffer[pipe->read_pos + i];
        }
        for (uint32_t i = 0; i < to_read - first_chunk; i++) {
            buf[first_chunk + i] = pipe->buffer[i];
        }
        pipe->read_pos = (pipe->read_pos + to_read) % PIPE_BUF_SIZE;
    }
    pipe->size -= to_read;

    spin_unlock_irqrestore(&pipe->lock, flags);
    wake_up(&pipe->write_wait);

    /* Solta o pin: o pipe pode morrer aqui (último close concorrente
     * já marcou dead). Nada do pipe é tocado depois deste ponto. */
    spin_lock_irqsave(&pipe->lock, &flags);
    int free_pipe = 0;
    pipe->refcount--;
    if (pipe->refcount == 0)
        free_pipe = 1;
    spin_unlock_irqrestore(&pipe->lock, flags);
    if (free_pipe)
        kfree(pipe);

    return to_read;
}

static uint32_t pipe_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    (void)offset;
    pipe_data_t *pipe = (pipe_data_t *)node->ptr;
    if (!pipe) return 0;
    if (size == 0) return 0;

    /* Chunked: um write maior que o buffer (ou maior que o espaço livre)
     * transmite em pedaços em vez de esperar o pedido inteiro caber de uma
     * vez — a condição antiga (size+size > BUF) travava para sempre quando
     * size sozinho excedia PIPE_BUF_SIZE, mesmo com o pipe vazio. */
    uint32_t written = 0;
    unsigned long flags;

    spin_lock_irqsave(&pipe->lock, &flags);
    if (pipe->dead) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        return (uint32_t)-1;
    }
    pipe->refcount++;   /* pin através dos sleeps abaixo */
    spin_unlock_irqrestore(&pipe->lock, flags);

    while (written < size) {
        spin_lock_irqsave(&pipe->lock, &flags);
        if (pipe->dead) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            break;
        }
        if (pipe->readers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            break;
        }
        while (pipe->size == PIPE_BUF_SIZE) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            wait_event_interruptible(pipe->write_wait,
                                     pipe->size < PIPE_BUF_SIZE);
            spin_lock_irqsave(&pipe->lock, &flags);
            if (pipe->dead || pipe->readers == 0) {
                spin_unlock_irqrestore(&pipe->lock, flags);
                goto out_release;
            }
        }

        uint32_t free_space = PIPE_BUF_SIZE - pipe->size;
        uint32_t chunk = size - written;
        if (chunk > free_space) chunk = free_space;
        uint32_t first_chunk = PIPE_BUF_SIZE - pipe->write_pos;
        const uint8_t *src = buf + written;
        if (chunk <= first_chunk) {
            for (uint32_t i = 0; i < chunk; i++) {
                pipe->buffer[pipe->write_pos + i] = src[i];
            }
            pipe->write_pos = (pipe->write_pos + chunk) % PIPE_BUF_SIZE;
        } else {
            for (uint32_t i = 0; i < first_chunk; i++) {
                pipe->buffer[pipe->write_pos + i] = src[i];
            }
            for (uint32_t i = 0; i < chunk - first_chunk; i++) {
                pipe->buffer[i] = src[first_chunk + i];
            }
            pipe->write_pos = (pipe->write_pos + chunk) % PIPE_BUF_SIZE;
        }
        pipe->size += chunk;
        written += chunk;

        spin_unlock_irqrestore(&pipe->lock, flags);
        wake_up(&pipe->read_wait);
    }

out_release:
    /* Solta o pin: o pipe pode morrer aqui. Nada do pipe é tocado
     * depois deste ponto. Parcial conta como entregue. */
    spin_lock_irqsave(&pipe->lock, &flags);
    int free_pipe = 0;
    pipe->refcount--;
    if (pipe->refcount == 0)
        free_pipe = 1;
    spin_unlock_irqrestore(&pipe->lock, flags);
    if (free_pipe)
        kfree(pipe);

    return written > 0 ? written : (uint32_t)-1;
}

static void pipe_open(vfs_node_t *node) {
    pipe_data_t *pipe = (pipe_data_t *)node->ptr;
    if (!pipe) return;

    unsigned long flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    if (node == pipe->read_node) {
        pipe->readers++;
    } else {
        pipe->writers++;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);
}

static void pipe_close(vfs_node_t *node) {
    pipe_data_t *pipe = (pipe_data_t *)node->ptr;
    if (!pipe) return;

    unsigned long flags;
    spin_lock_irqsave(&pipe->lock, &flags);
    if (pipe->dead) {
        /* Ponta já liberada (teardown anterior): sem double-decrement. */
        spin_unlock_irqrestore(&pipe->lock, flags);
        return;
    }

    if (node == pipe->read_node) {
        if (pipe->readers > 0)
            pipe->readers--;
        if (pipe->readers == 0) {
            wake_up(&pipe->write_wait);
        }
    } else {
        if (pipe->writers > 0)
            pipe->writers--;
        if (pipe->writers == 0) {
            wake_up(&pipe->read_wait);
        }
    }

    if (pipe->readers == 0 && pipe->writers == 0) {
        /* Último close: marca dead, acorda qualquer dormente restante,
         * libera os nodes e solta as 2 refs das pontas. Dorminhocos com
         * pin veem dead e soltam o pin (o último libera o pipe). */
        pipe->dead = 1;
        vfs_node_t *rn = pipe->read_node;
        vfs_node_t *wn = pipe->write_node;
        pipe->read_node = NULL;
        pipe->write_node = NULL;
        wake_up(&pipe->read_wait);
        wake_up(&pipe->write_wait);
        pipe->refcount -= 2;
        int free_pipe = (pipe->refcount == 0);
        spin_unlock_irqrestore(&pipe->lock, flags);
        if (rn) kfree(rn);
        if (wn && wn != rn) kfree(wn);
        if (free_pipe)
            kfree(pipe);
        return;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);
}

vfs_node_t *pipe_create(vfs_node_t **write_end) {
    pipe_data_t *pipe = (pipe_data_t *)kmalloc(sizeof(pipe_data_t));
    if (!pipe) return NULL;

    for (int i = 0; i < PIPE_BUF_SIZE; i++) pipe->buffer[i] = 0;
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->size = 0;
    pipe->readers = 0;
    pipe->writers = 0;
    pipe->refcount = 2;   /* uma por ponta (read_node + write_node) */
    pipe->dead = 0;
    init_waitqueue_head(&pipe->read_wait);
    init_waitqueue_head(&pipe->write_wait);
    spinlock_init(&pipe->lock.lock);

    vfs_node_t *read_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    vfs_node_t *write_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!read_node || !write_node) {
        if (read_node) kfree(read_node);
        if (write_node) kfree(write_node);
        kfree(pipe);
        return NULL;
    }

    for (int i = 0; i < VFS_MAX_NAME; i++) {
        read_node->name[i] = 0;
        write_node->name[i] = 0;
    }
    const char *pipe_name = "pipe";
    for (int i = 0; pipe_name[i]; i++) {
        read_node->name[i] = pipe_name[i];
        write_node->name[i] = pipe_name[i];
    }

    read_node->flags = VFS_FILE | VFS_PIPE;
    write_node->flags = VFS_FILE | VFS_PIPE;
    read_node->inode = 0;
    write_node->inode = 0;
    read_node->length = PIPE_BUF_SIZE;
    write_node->length = PIPE_BUF_SIZE;
    read_node->uid = write_node->uid = 0;
    read_node->gid = write_node->gid = 0;

    read_node->read = pipe_read;
    read_node->write = NULL;
    read_node->open = pipe_open;
    read_node->close = pipe_close;
    read_node->readdir = NULL;
    read_node->finddir = NULL;
    read_node->ptr = (struct vfs_node *)pipe;

    write_node->read = NULL;
    write_node->write = pipe_write;
    write_node->open = pipe_open;
    write_node->close = pipe_close;
    write_node->readdir = NULL;
    write_node->finddir = NULL;
    write_node->ptr = (struct vfs_node *)pipe;

    pipe->read_node = read_node;
    pipe->write_node = write_node;

    *write_end = write_node;
    return read_node;
}

/* ------------------------------------------------------------------ */
/* Selftest de boot: round-trip de 8 KiB num único write (> PIPE_BUF)  */
/* ------------------------------------------------------------------ */

#define PIPE_TEST_TOTAL 8192

static uint8_t pipe_test_src[PIPE_TEST_TOTAL];
static uint8_t pipe_test_dst[PIPE_TEST_TOTAL];
static vfs_node_t *pipe_test_rd;
static volatile uint32_t pipe_test_read_total;
static volatile int pipe_test_reader_done;

static void pipe_close_test(void);

static void pipe_test_reader(void) {
    while (pipe_test_read_total < PIPE_TEST_TOTAL) {
        uint32_t got = pipe_test_rd->read(pipe_test_rd, 0,
            PIPE_TEST_TOTAL - pipe_test_read_total,
            pipe_test_dst + pipe_test_read_total);
        if (got == 0)
            break;  /* EOF: writer fechou */
        pipe_test_read_total += got;
    }
    pipe_test_reader_done = 1;
}

void pipe_test(void) {
    serial_print("[PIPE] Starting pipe tests (8KiB round-trip)...\n");

    for (uint32_t i = 0; i < PIPE_TEST_TOTAL; i++) {
        pipe_test_src[i] = (uint8_t)(i * 31 + 7);
        pipe_test_dst[i] = 0;
    }
    pipe_test_read_total = 0;
    pipe_test_reader_done = 0;

    vfs_node_t *wr = NULL;
    vfs_node_t *rd = pipe_create(&wr);
    if (!rd || !wr) {
        serial_print("[PIPE] FAIL: pipe_create\n");
        return;
    }
    pipe_test_rd = rd;
    rd->open(rd);
    wr->open(wr);

    /* Reader roda em task própria: o write de 8192 bloqueia quando o buffer
     * de 4096 enche e só progride quando o reader drena (é exatamente o
     * cenário que travava para sempre no código antigo). A reader task fica
     * SUSPENDED após o teste (sem reaper — gap de lifecycle documentado). */
    task_struct_t *reader = task_create(pipe_test_reader, 0);
    if (!reader) {
        serial_print("[PIPE] FAIL: task_create reader\n");
        rd->close(rd);
        wr->close(wr);
        return;
    }

    uint32_t w = wr->write(wr, 0, PIPE_TEST_TOTAL, pipe_test_src);

    uint64_t start = timer_get_ticks();
    while (!pipe_test_reader_done && timer_get_ticks() - start < 500)
        schedule();

    int ok = (w == PIPE_TEST_TOTAL && pipe_test_reader_done &&
              pipe_test_read_total == PIPE_TEST_TOTAL);
    for (uint32_t i = 0; ok && i < PIPE_TEST_TOTAL; i++) {
        if (pipe_test_dst[i] != pipe_test_src[i])
            ok = 0;
    }

    rd->close(rd);
    wr->close(wr);  /* última close libera pipe + nodes */

    if (ok)
        serial_print("[PIPE] ALL CHECKS PASSED (8KiB chunked round-trip)\n");
    else
        serial_print("[PIPE] TESTS FAILED\n");

    pipe_close_test();
}

/* Teardown com waiter bloqueado: reader dorme no pipe vazio (writers=1);
 * main fecha as duas pontas por baixo dele. O reader deve acordar com
 * EOF (0) via flag dead — sem UAF — e o pipe ser liberado exatamente
 * uma vez (provado recriando/usando/destruindo outro pipe depois). */
static vfs_node_t *pctest_rd;
static uint8_t pctest_buf[16];
static volatile int pctest_entered;
static volatile int pctest_done;
static volatile uint32_t pctest_got;

static void pipe_close_reader(void) {
    pctest_entered = 1;
    uint32_t got = pctest_rd->read(pctest_rd, 0, sizeof(pctest_buf), pctest_buf);
    pctest_got = got;
    pctest_done = 1;
}

static void pipe_close_test(void) {
    serial_print("[PIPE] close-during-read: waiter bloqueado + teardown...\n");
    pctest_entered = 0;
    pctest_done = 0;
    pctest_got = 0xDEADu;

    vfs_node_t *wr = NULL;
    vfs_node_t *rd = pipe_create(&wr);
    if (!rd || !wr) {
        serial_print("[PIPE] FAIL: close-test create\n");
        return;
    }
    pctest_rd = rd;
    rd->open(rd);
    wr->open(wr);

    task_struct_t *reader = task_create(pipe_close_reader, 0);
    if (!reader) {
        serial_print("[PIPE] FAIL: close-test spawn\n");
        rd->close(rd);
        wr->close(wr);
        return;
    }
    unsigned it = 200000;
    while (it-- && !pctest_entered)
        schedule();
    if (!pctest_entered) {
        serial_print("[PIPE] FAIL: close-test reader nao entrou\n");
        rd->close(rd);
        wr->close(wr);
        return;
    }
    for (unsigned i = 0; i < 50000; i++)
        schedule();   /* garante o bloqueio dentro do wait */

    rd->close(rd);
    wr->close(wr);    /* última: dead + free nodes; reader acorda com EOF */

    unsigned w = 200000;
    while (w-- && !pctest_done)
        schedule();
    int ok = (pctest_done && pctest_got == 0);

    /* Heap são após free sob waiter: recria, usa e destrói outro pipe. */
    vfs_node_t *wr2 = NULL;
    vfs_node_t *rd2 = pipe_create(&wr2);
    if (rd2 && wr2) {
        uint8_t tmp[4] = {'t', 'e', 's', 't'};
        uint8_t out[4] = {0, 0, 0, 0};
        rd2->open(rd2);
        wr2->open(wr2);
        ok = ok && (wr2->write(wr2, 0, 4, tmp) == 4);
        ok = ok && (rd2->read(rd2, 0, 4, out) == 4);
        ok = ok && (out[0] == 't' && out[3] == 't');
        rd2->close(rd2);
        wr2->close(wr2);
    } else {
        ok = 0;
    }

    if (ok)
        serial_print("[PIPE] ALL CHECKS PASSED (close-during-read EOF + heap sao)\n");
    else
        serial_print("[PIPE] TESTS FAILED (close-during-read)\n");
}