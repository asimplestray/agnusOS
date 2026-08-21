#include <vfs.h>
#include <kheap.h>
#include <wait.h>
#include <screen.h>
#include <spinlock.h>
#include <task.h>

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
} pipe_data_t;

static uint32_t pipe_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)offset;
    pipe_data_t *pipe = (pipe_data_t *)node->ptr;
    if (!pipe) return 0;

    unsigned long flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    while (pipe->size == 0) {
        if (pipe->writers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&pipe->lock, flags);
        wait_event_interruptible(pipe->read_wait, pipe->size > 0 || pipe->writers == 0);
        spin_lock_irqsave(&pipe->lock, &flags);
        if (pipe->size == 0 && pipe->writers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return 0;
        }
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

    return to_read;
}

static uint32_t pipe_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    (void)offset;
    pipe_data_t *pipe = (pipe_data_t *)node->ptr;
    if (!pipe) return 0;

    unsigned long flags;
    spin_lock_irqsave(&pipe->lock, &flags);

    if (pipe->readers == 0) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        return (uint32_t)-1;
    }

    while (pipe->size + size > PIPE_BUF_SIZE) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        wait_event_interruptible(pipe->write_wait, pipe->size + size <= PIPE_BUF_SIZE);
        spin_lock_irqsave(&pipe->lock, &flags);
        if (pipe->readers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            return (uint32_t)-1;
        }
    }

    uint32_t free_space = PIPE_BUF_SIZE - pipe->size;
    uint32_t to_write = size < free_space ? size : free_space;
    uint32_t first_chunk = PIPE_BUF_SIZE - pipe->write_pos;
    if (to_write <= first_chunk) {
        for (uint32_t i = 0; i < to_write; i++) {
            pipe->buffer[pipe->write_pos + i] = buf[i];
        }
        pipe->write_pos = (pipe->write_pos + to_write) % PIPE_BUF_SIZE;
    } else {
        for (uint32_t i = 0; i < first_chunk; i++) {
            pipe->buffer[pipe->write_pos + i] = buf[i];
        }
        for (uint32_t i = 0; i < to_write - first_chunk; i++) {
            pipe->buffer[i] = buf[first_chunk + i];
        }
        pipe->write_pos = (pipe->write_pos + to_write) % PIPE_BUF_SIZE;
    }
    pipe->size += to_write;

    spin_unlock_irqrestore(&pipe->lock, flags);
    wake_up(&pipe->read_wait);

    return to_write;
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

    if (node == pipe->read_node) {
        pipe->readers--;
        if (pipe->readers == 0) {
            wake_up(&pipe->write_wait);
        }
    } else {
        pipe->writers--;
        if (pipe->writers == 0) {
            wake_up(&pipe->read_wait);
        }
    }

    if (pipe->readers == 0 && pipe->writers == 0) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        if (pipe->read_node) kfree(pipe->read_node);
        if (pipe->write_node) kfree(pipe->write_node);
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