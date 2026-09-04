/*
 * ProcFS — dynamic /proc filesystem built on top of RamFS
 *
 * Content is generated on every read, so `cat /proc/meminfo` always shows
 * fresh numbers:
 *
 *   /proc/meminfo       — physical RAM usage + dirty disk sectors
 *   /proc/uptime        — seconds since boot
 *   /proc/<pid>/status  — per-process status
 *
 * The process directories are created on demand by /proc finddir().
 */

#include <procfs.h>
#include <ramfs.h>
#include <vfs.h>
#include <kheap.h>
#include <task.h>
#include <pmm.h>
#include <timer.h>
#include <ata.h>
#include <screen.h>
#include <assign.h>
#include <msgport.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define PROC_DATA_SIZE 4096
#define PROC_MAX_PIDS  32

/* A dynamic procfs file: a ramfs node whose data is regenerated on read */
typedef struct proc_file {
    ramfs_node_t rn;
    uint32_t     pid;    /* bound pid for status files (0 = unbound) */
    int (*generate)(struct proc_file *pf, char *buf, int max);
} proc_file_t;

extern task_struct_t *task_list;

/* ---- minimal formatter ( %s %d %u %c %%. ) ------------------------------- */

static int mt_sprintf(char *out, int max, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    int o = 0;
    while (*fmt && o < max - 1) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && o < max - 1) out[o++] = *s++;
            } else if (*fmt == 'd' || *fmt == 'u') {
                long long v = (*fmt == 'd') ? (long long)va_arg(ap, int)
                                            : (long long)va_arg(ap, unsigned int);
                char dec[24];
                int i = 0;
                if (v < 0) { out[o++] = '-'; v = -v; }
                if (v == 0) dec[i++] = '0';
                while (v > 0) { dec[i++] = '0' + (v % 10); v /= 10; }
                while (i > 0 && o < max - 1) out[o++] = dec[--i];
            } else if (*fmt == 'c') {
                out[o++] = (char)va_arg(ap, int);
            } else if (*fmt == '%') {
                out[o++] = '%';
            }
        } else {
            out[o++] = *fmt;
        }
        fmt++;
    }
    out[o] = '\0';
    va_end(ap);
    return o;
}

/* ---- content generators --------------------------------------------------- */

static int gen_meminfo(struct proc_file *pf, char *buf, int max) {
    (void)pf;
    uint64_t total = pmm_get_total_memory();
    uint64_t free  = pmm_get_free_memory();
    return mt_sprintf(buf, max,
        "MemTotal:      %u kB\n"
        "MemFree:       %u kB\n"
        "MemUsed:       %u kB\n"
        "DirtySectors:  %u\n",
        (unsigned int)(total / 1024),
        (unsigned int)(free / 1024),
        (unsigned int)((total - free) / 1024),
        ata_dirty_count());
}

static int gen_uptime(struct proc_file *pf, char *buf, int max) {
    (void)pf;
    uint64_t ticks = timer_get_ticks();
    uint64_t secs  = ticks / 100;
    uint64_t tenths = ticks % 100;
    return mt_sprintf(buf, max, "%u.%02u\n",
                      (unsigned int)secs, (unsigned int)tenths);
}

static int gen_assigns(struct proc_file *pf, char *buf, int max) {
    (void)pf;
    return assign_dump(buf, max);
}

static int gen_ports(struct proc_file *pf, char *buf, int max) {
    (void)pf;
    return msgport_dump(buf, max);
}

static task_struct_t *task_by_pid(uint64_t pid) {
    if (!task_list) return NULL;
    task_struct_t *t = task_list;
    do {
        if (t->pid == pid) return t;
        t = t->next;
    } while (t != task_list);
    return NULL;
}

static const char *task_state_name(task_state_t st) {
    switch (st) {
        case TASK_STATE_RUNNING:        return "running";
        case TASK_STATE_INTERRUPTIBLE:  return "interruptible";
        case TASK_STATE_UNINTERRUPTIBLE:return "uninterruptible";
        case TASK_STATE_STOPPED:        return "stopped";
        case TASK_STATE_ZOMBIE:         return "zombie";
        default:                        return "unknown";
    }
}

static int gen_status(struct proc_file *pf, char *buf, int max) {
    task_struct_t *t = task_by_pid(pf->pid);
    if (!t) {
        return mt_sprintf(buf, max, "Pid:\t%u\t(exited)\n", pf->pid);
    }
    return mt_sprintf(buf, max,
        "Pid:\t%u\n"
        "Ppid:\t%u\n"
        "State:\t%s\n"
        "Ticks:\t%u\n"
        "Priority:\t%u\n",
        (unsigned int)t->pid,
        t->parent ? (unsigned int)t->parent->pid : 0u,
        task_state_name(t->state),
        (unsigned int)t->ticks,
        (unsigned int)t->priority);
}

/* ---- dynamic read --------------------------------------------------------- */

static uint32_t procfs_read(vfs_node_t *node, uint32_t offset,
                            uint32_t size, uint8_t *buf) {
    proc_file_t *pf = (proc_file_t *)node;
    if (!pf->generate || !pf->rn.data) return 0;

    if (offset == 0) {
        int len = pf->generate(pf, (char *)pf->rn.data, PROC_DATA_SIZE);
        if (len < 0) len = 0;
        pf->rn.data_size = (uint32_t)len;
        pf->rn.vfs.length = (uint32_t)len;
    }
    if (offset >= pf->rn.data_size) return 0;

    uint32_t avail = pf->rn.data_size - offset;
    if (size > avail) size = avail;
    for (uint32_t i = 0; i < size; i++) buf[i] = pf->rn.data[offset + i];
    return size;
}

/* ---- node constructors ----------------------------------------------------- */

static proc_file_t *proc_make_file(const char *name, int (*gen)(struct proc_file *, char *, int)) {
    proc_file_t *pf = (proc_file_t *)kmalloc(sizeof(proc_file_t));
    if (!pf) return NULL;

    for (uint32_t i = 0; i < sizeof(pf->rn); i++) ((uint8_t *)pf)[i] = 0;

    int n = 0;
    while (name[n] && n < VFS_MAX_NAME - 1) {
        pf->rn.vfs.name[n] = name[n];
        n++;
    }
    pf->rn.vfs.name[n] = '\0';

    pf->rn.vfs.flags  = VFS_FILE;
    pf->rn.vfs.read  = procfs_read;
    pf->rn.vfs.length = 0;
    pf->rn.data = (uint8_t *)kmalloc(PROC_DATA_SIZE);
    pf->rn.data_size = 0;
    pf->generate = gen;
    return pf;
}

/* ---- /proc dynamic directory ---------------------------------------------- */

static vfs_node_t *proc_pid_dir(uint64_t pid);

static vfs_dirent_t *proc_pid_readdir(vfs_node_t *node, uint32_t index) {
    ramfs_node_t *dir = (ramfs_node_t *)node;
    vfs_node_t *child = ramfs_readdir_node(dir, index);
    static vfs_dirent_t de;
    if (!child) return NULL;
    int i = 0;
    while (child->name[i] && i < VFS_MAX_NAME - 1) {
        de.name[i] = child->name[i];
        i++;
    }
    de.name[i] = '\0';
    de.inode = child->inode;
    return &de;
}

static vfs_node_t *proc_pid_finddir(vfs_node_t *node, const char *name) {
    return ramfs_finddir_node((ramfs_node_t *)node, name);
}

static vfs_dirent_t *proc_root_readdir(vfs_node_t *node, uint32_t index) {
    static vfs_dirent_t de;
    ramfs_node_t *rn = (ramfs_node_t *)node;

    /* First the two metadata files */
    if (index < 2) {
        static const char *names[2] = { "meminfo", "uptime" };
        int i = 0;
        while (names[index][i] && i < VFS_MAX_NAME - 1) {
            de.name[i] = names[index][i];
            i++;
        }
        de.name[i] = '\0';
        de.inode = (uint32_t)(index + 1);
        return &de;
    }

    /* Then the cached children (meminfo/uptime are already counted above,
     * so only dynamic pid dirs remain in the ramfs children array) */
    vfs_node_t *child = ramfs_readdir_node(rn, index - 2);
    if (!child) return NULL;

    int i = 0;
    while (child->name[i] && i < VFS_MAX_NAME - 1) {
        de.name[i] = child->name[i];
        i++;
    }
    de.name[i] = '\0';
    de.inode = child->inode;
    return &de;
}

static vfs_node_t *proc_root_finddir(vfs_node_t *node, const char *name) {
    ramfs_node_t *rn = (ramfs_node_t *)node;

    /* Match the static files through the normal ramfs tree */
    vfs_node_t *cached = ramfs_finddir_node(rn, name);
    if (cached) return cached;

    /* Otherwise a numeric name means /proc/<pid> */
    if (name[0] < '0' || name[0] > '9') return NULL;

    uint64_t pid = 0;
    for (int i = 0; name[i] && i < 16; i++) {
        if (name[i] < '0' || name[i] > '9') return NULL;
        pid = pid * 10 + (name[i] - '0');
    }
    if (pid == 0) return NULL;
    return proc_pid_dir(pid);
}

/* Keep a small cache of created pid directories so repeated lookups
 * (e.g. stat after open) return the same node. */
static proc_file_t *proc_pid_dirs[PROC_MAX_PIDS];
static uint32_t proc_num_pid_dirs = 0;

static uint32_t proc_next_inode = 0xE0000;

static vfs_node_t *proc_pid_dir(uint64_t pid) {
    /* Already cached? */
    for (uint32_t i = 0; i < proc_num_pid_dirs; i++) {
        if (proc_pid_dirs[i]->pid == pid)
            return &proc_pid_dirs[i]->rn.vfs;
    }
    if (!task_by_pid(pid)) return NULL;
    if (proc_num_pid_dirs >= PROC_MAX_PIDS) return NULL;

    char pid_str[16];
    mt_sprintf(pid_str, sizeof(pid_str), "%u", (unsigned int)pid);

    proc_file_t *dir = (proc_file_t *)kmalloc(sizeof(proc_file_t));
    if (!dir) return NULL;
    for (uint32_t i = 0; i < sizeof(dir->rn); i++) ((uint8_t *)dir)[i] = 0;

    int n = 0;
    while (pid_str[n] && n < VFS_MAX_NAME - 1) {
        dir->rn.vfs.name[n] = pid_str[n];
        n++;
    }
    dir->rn.vfs.name[n] = '\0';
    dir->rn.vfs.flags   = VFS_DIRECTORY;
    dir->rn.vfs.inode   = proc_next_inode++;
    dir->rn.vfs.readdir = proc_pid_readdir;
    dir->rn.vfs.finddir = proc_pid_finddir;

    proc_file_t *status = proc_make_file("status", gen_status);
    if (!status) {
        kfree(dir);
        return NULL;
    }
    status->pid = (uint32_t)pid;
    ramfs_attach(&dir->rn, &status->rn);

    proc_pid_dirs[proc_num_pid_dirs++] = dir;
    return &dir->rn.vfs;
}

/* ---- init ----------------------------------------------------------------- */

void procfs_init(void) {
    extern vfs_node_t *vfs_root;
    if (!vfs_root) return;

    ramfs_node_t *root = (ramfs_node_t *)vfs_root;

    ramfs_node_t *proc = ramfs_mkdir_node("proc");
    if (!proc) return;
    ramfs_attach(root, proc);

    proc_file_t *meminfo = proc_make_file("meminfo", gen_meminfo);
    proc_file_t *uptime  = proc_make_file("uptime",  gen_uptime);
    proc_file_t *assigns = proc_make_file("assigns", gen_assigns);
    proc_file_t *ports   = proc_make_file("ports",   gen_ports);
    if (meminfo) ramfs_attach(proc, &meminfo->rn);
    if (uptime)  ramfs_attach(proc, &uptime->rn);
    if (assigns) ramfs_attach(proc, &assigns->rn);
    if (ports)   ramfs_attach(proc, &ports->rn);

    /* Make /proc dynamic: pid dirs are resolved on demand */
    proc->vfs.readdir = proc_root_readdir;
    proc->vfs.finddir = proc_root_finddir;

    screen_log("OK", COLOR_LIGHT_GREEN, "ProcFS montado em /proc (dinamico).");
}