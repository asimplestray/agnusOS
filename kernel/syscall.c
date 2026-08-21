#include <syscall.h>
#include <task.h>
#include <screen.h>
#include <kheap.h>
#include <vmm.h>
#include <pmm.h>
#include <timer.h>
#include <vfs.h>
#include <elf.h>
#include <gdt.h>
#include <keyboard.h>
#include <rtc.h>
#include <panic.h>
#include <idt.h>
#include <ata.h>
#include <tty.h>
#include <string.h>

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

static int64_t (*syscall_table[NR_SYSCALLS])(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, struct interrupt_frame *) = {0};

void syscall_init(void) {
    syscall_table[SYS_EXIT]           = (void *)sys_exit;
    syscall_table[SYS_FORK]           = (void *)sys_fork;
    syscall_table[SYS_READ]           = (void *)sys_read;
    syscall_table[SYS_WRITE]          = (void *)sys_write;
    syscall_table[SYS_OPEN]           = (void *)sys_open;
    syscall_table[SYS_CLOSE]          = (void *)sys_close;
    syscall_table[SYS_WAITPID]        = (void *)sys_waitpid;
    syscall_table[SYS_EXECVE]         = (void *)sys_execve;
    syscall_table[SYS_BRK]            = (void *)sys_brk;
    syscall_table[SYS_MMAP]           = (void *)sys_mmap;
    syscall_table[SYS_MUNMAP]         = (void *)sys_munmap;
    syscall_table[SYS_IOCTL]          = (void *)sys_ioctl;
    syscall_table[SYS_GETPID]         = (void *)sys_getpid;
    syscall_table[SYS_YIELD]          = (void *)sys_yield;
    syscall_table[SYS_NANOSLEEP]      = (void *)sys_nanosleep;
    syscall_table[SYS_GETTIME]        = (void *)sys_gettime;
    syscall_table[SYS_SPAWN]          = (void *)sys_spawn;
    syscall_table[SYS_RT_SIGACTION]   = (void *)sys_rt_sigaction;
    syscall_table[SYS_RT_SIGPROCMASK] = (void *)sys_rt_sigprocmask;
    syscall_table[SYS_RT_SIGRETURN]   = (void *)sys_rt_sigreturn;
    syscall_table[SYS_KILL]           = (void *)sys_kill;
    syscall_table[SYS_PIPE]           = (void *)sys_pipe;
    syscall_table[SYS_LSEEK]          = (void *)sys_lseek;
    syscall_table[SYS_STAT]           = (void *)sys_stat;
    syscall_table[SYS_CLOCK_GETTIME]  = (void *)sys_clock_gettime;
    syscall_table[SYS_SYSLOG]         = (void *)sys_syslog;
    syscall_table[SYS_GET_ERRNO]      = (void *)sys_get_errno;
    syscall_table[SYS_SET_ERRNO]      = (void *)sys_set_errno;
    syscall_table[SYS_SETPGID]        = (void *)sys_setpgid;
    syscall_table[SYS_GETPGID]        = (void *)sys_getpgid;
    syscall_table[SYS_TCSETPGRP]      = (void *)sys_tcsetpgrp;
    syscall_table[SYS_TCGETPGRP]      = (void *)sys_tcgetpgrp;
    syscall_table[SYS_MKDIR]          = (void *)sys_mkdir;
    syscall_table[SYS_RMDIR]          = (void *)sys_rmdir;
    syscall_table[SYS_UNLINK]         = (void *)sys_unlink;
    syscall_table[SYS_CHDIR]          = (void *)sys_chdir;
    syscall_table[SYS_FCHDIR]         = (void *)sys_fchdir;
    syscall_table[SYS_GETCWD]         = (void *)sys_getcwd;
    syscall_table[SYS_RENAME]         = (void *)sys_rename;
    syscall_table[SYS_GETDENTS]       = (void *)sys_getdents;
    syscall_table[SYS_SOCKET]         = (void *)sys_socket;
    syscall_table[SYS_BIND]           = (void *)sys_bind;
    syscall_table[SYS_SENDTO]         = (void *)sys_sendto;
    syscall_table[SYS_RECVFROM]       = (void *)sys_recvfrom;
    syscall_table[SYS_SOCK_CLOSE]     = (void *)sys_sock_close;
    syscall_table[SYS_FSYNC]          = (void *)sys_fsync;
}

void syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, struct interrupt_frame *frame) {
    if (num >= NR_SYSCALLS || !syscall_table[num]) {
        if (current) current->errno_val = ENOSYS;
        current->rax = (uint64_t)(-ENOSYS);
        return;
    }
    int64_t ret = syscall_table[num](a1, a2, a3, a4, a5, a6, frame);
    if (current && ret < 0) {
        current->errno_val = -ret;
        current->rax = (uint64_t)ret;
    } else {
        current->rax = (uint64_t)ret;
    }
}

/* -------------------------------------------------------------------------
 * Process management
 * ---------------------------------------------------------------------- */

int64_t sys_exit(int64_t code, struct interrupt_frame *frame) {
    (void)frame;
    task_exit((int)code);
    return 0;
}

int64_t sys_fork(struct interrupt_frame *frame) {
    task_struct_t *child = task_create((void (*)(void))current->rip, 0);
    if (!child) return -1;

    /* Since task_create shared current->mm, decrement it to isolate */
    if (child->mm) {
        child->mm->refcount--;
    }

    /* Allocate a new isolated mm_struct for the child */
    child->mm = (mm_struct_t *)kmalloc(sizeof(mm_struct_t));
    if (!child->mm) return -1;

    child->mm->start_code = current->mm->start_code;
    child->mm->end_code = current->mm->end_code;
    child->mm->start_data = current->mm->start_data;
    child->mm->end_data = current->mm->end_data;
    child->mm->start_brk = current->mm->start_brk;
    child->mm->brk = current->mm->brk;
    child->mm->start_stack = current->mm->start_stack;
    spinlock_init(&child->mm->lock);
    child->mm->refcount = 1;

    /* Clone page table with COW */
    child->mm->pml4_phys = vmm_clone_user_pml4(current->mm->pml4_phys);
    if (!child->mm->pml4_phys) return -1;

    /* Mark both parent and child user pages as COW */
    vmm_mark_cow_user_pages(current->mm->pml4_phys);
    vmm_mark_cow_user_pages(child->mm->pml4_phys);

    /* Flush TLB for current process */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    child->rax = 0;
    child->rbx = current->rbx;
    child->rcx = current->rcx;
    child->rdx = current->rdx;
    child->rsi = current->rsi;
    child->rdi = current->rdi;
    child->rbp = current->rbp;
    child->r8  = current->r8;
    child->r9  = current->r9;
    child->r10 = current->r10;
    child->r11 = current->r11;
    child->r12 = current->r12;
    child->r13 = current->r13;
    child->r14 = current->r14;
    child->r15 = current->r15;
    child->rsp = current->rsp;
    child->rflags = current->rflags;
    child->fs_base = current->fs_base;
    child->gs_base = current->gs_base;
    child->cwd = current->cwd;

    /* Save parent's FPU state to child */
    fpu_save(child->fxsave_area);
    child->fpu_used = true;

    /* Set child's return value to 0 in its frame */
    if (frame) {
        frame->rax = 0;
    }

    return (int64_t)child->pid;
}

int64_t sys_getpid(struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)(current ? current->pid : 0);
}

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4

#define WIFEXITED(status) (((status) & 0xFF) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0 && ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status) ((status) & 0x7F)
#define WIFSTOPPED(status) (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status) (((status) >> 8) & 0xFF)
#define WIFCONTINUED(status) (((status) & 0xFFFF) == 0xFFFF)

int64_t sys_waitpid(int64_t pid, int64_t *status, int64_t options, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -ECHILD;
    
    int found_child = 0;
    task_struct_t *child = NULL;
    task_struct_t *prev = NULL;
    
    while (1) {
        found_child = 0;
        
        if (pid == -1) {
            child = current->children;
            while (child) {
                if (child->state == TASK_STATE_ZOMBIE) {
                    found_child = 1;
                    break;
                }
                if ((options & WUNTRACED) && child->state == TASK_STATE_STOPPED) {
                    found_child = 1;
                    break;
                }
                child = child->next_sibling;
            }
        } else if (pid > 0) {
            child = current->children;
            while (child) {
                if (child->pid == (uint64_t)pid) {
                    if (child->state == TASK_STATE_ZOMBIE) {
                        found_child = 1;
                    } else if ((options & WUNTRACED) && child->state == TASK_STATE_STOPPED) {
                        found_child = 1;
                    }
                    break;
                }
                child = child->next_sibling;
            }
        }
        
        if (found_child) {
            uint64_t ret_pid = child->pid;
            int exit_code = child->exit_code;
            
            if (status) {
                int wstatus;
                if (child->state == TASK_STATE_ZOMBIE) {
                    wstatus = (exit_code << 8) & 0xFF00;
                } else if (child->state == TASK_STATE_STOPPED) {
                    wstatus = 0x7F | (exit_code << 8);
                } else {
                    wstatus = 0xFFFF;
                }
                *status = wstatus;
            }
            
            /* Only remove zombie children, not stopped ones */
            if (child->state == TASK_STATE_ZOMBIE) {
                if (prev) {
                    prev->next_sibling = child->next_sibling;
                } else {
                    current->children = child->next_sibling;
                }
                if (child->next_sibling) {
                    child->next_sibling->prev_sibling = prev;
                }
                
                child->prev->next = child->next;
                child->next->prev = child->prev;
                
                if (child->mm) {
                    child->mm->refcount--;
                    if (child->mm->refcount == 0) {
                        extern uint64_t kernel_pml4_phys;
                        if (child->mm->pml4_phys != 0 && child->mm->pml4_phys != kernel_pml4_phys) {
                            vmm_free_pml4(child->mm->pml4_phys);
                        }
                        kfree(child->mm);
                    }
                }
                if (child->files) {
                    child->files->count--;
                }
                kfree((void *)(child->kernel_stack - 16384));
                kfree(child);
            }
            
            return (int64_t)ret_pid;
        }
        
        if (options & WNOHANG) {
            return 0;
        }
        
        wait_event(current->wait_chldexit, 0);
    }
}

/* -------------------------------------------------------------------------
 * Errno support
 * ---------------------------------------------------------------------- */

int64_t sys_get_errno(struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return 0;
    return (int64_t)current->errno_val;
}

int64_t sys_set_errno(int64_t errno_val, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -1;
    current->errno_val = (int)errno_val;
    return 0;
}

/* -------------------------------------------------------------------------
 * Process groups + job control
 * ---------------------------------------------------------------------- */

int64_t sys_setpgid(int64_t pid, int64_t pgid, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -1;
    
    task_struct_t *target = NULL;
    if (pid == 0) {
        target = current;
    } else {
        task_struct_t *t = task_list;
        do {
            if (t->pid == (uint64_t)pid) {
                target = t;
                break;
            }
            t = t->next;
        } while (t != task_list);
    }
    
    if (!target) return -ESRCH;
    
    if (target != current && target->parent != current) {
        return -EPERM;
    }
    
    if (pgid == 0) {
        pgid = target->pid;
    }
    
    if (pgid < 0) return -EINVAL;
    
    target->pgid = (uint64_t)pgid;
    return 0;
}

int64_t sys_getpgid(int64_t pid, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -1;
    
    task_struct_t *target = NULL;
    if (pid == 0) {
        target = current;
    } else {
        task_struct_t *t = task_list;
        do {
            if (t->pid == (uint64_t)pid) {
                target = t;
                break;
            }
            t = t->next;
        } while (t != task_list);
    }
    
    if (!target) return -ESRCH;
    
    return (int64_t)target->pgid;
}

/* TTY process group - implemented with tty driver */
int64_t sys_tcsetpgrp(int64_t fd, int64_t pgid, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;
    
    if (!console_tty) return -ENOTTY;
    
    /* Check if the calling process has permission */
    if (pgid <= 0) return -EINVAL;
    
    /* Verify the process group exists */
    task_struct_t *t = task_list;
    int found = 0;
    do {
        if (t->pgid == (uint64_t)pgid) {
            found = 1;
            break;
        }
        t = t->next;
    } while (t != task_list);
    
    if (!found) return -EPERM;
    
    console_tty->fg_pgrp = (int)pgid;
    return 0;
}

int64_t sys_tcgetpgrp(int64_t fd, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;
    
    if (!console_tty) return -ENOTTY;
    
    return (int64_t)console_tty->fg_pgrp;
}

int64_t sys_yield(struct interrupt_frame *frame) {
    (void)frame;
    extern volatile uint64_t need_resched;
    need_resched = 1;
    return 0;
}

static void nanosleep_callback(uint64_t task_ptr) {
    task_struct_t *task = (task_struct_t *)task_ptr;
    task->state = TASK_STATE_RUNNING;
}

int64_t sys_nanosleep(const struct timespec *req, struct timespec *rem, struct interrupt_frame *frame) {
    (void)frame;
    if (!req) return -1;
    (void)rem;

    uint64_t ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    if (ms == 0) return 0;

    uint64_t ticks = ms / 10; /* 100Hz = 10ms per tick */
    if (ticks == 0) ticks = 1;

    timer_entry_t timer;
    timer.expires = timer_get_ticks() + ticks;
    timer.function = nanosleep_callback;
    timer.data = (uint64_t)current;

    current->state = TASK_STATE_UNINTERRUPTIBLE;
    timer_add(&timer);

    schedule();

    return 0;
}

int64_t sys_gettime(struct timespec *ts, struct interrupt_frame *frame) {
    (void)frame;
    if (!ts) return -1;
    if (!rtc_is_initialized()) return -1;

    uint64_t ns = rtc_get_epoch_nanoseconds();
    ts->tv_sec = (int64_t)(ns / 1000000000ULL);
    ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
    return 0;
}

int64_t sys_clock_gettime(int64_t clk_id, struct timespec *ts, struct interrupt_frame *frame) {
    (void)frame;
    if (!ts) return -1;
    if (!rtc_is_initialized()) return -1;

    (void)clk_id;
    uint64_t ns = rtc_get_epoch_nanoseconds();
    ts->tv_sec = (int64_t)(ns / 1000000000ULL);
    ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
    return 0;
}

int64_t sys_syslog(int type, char *buf, int len, struct interrupt_frame *frame) {
    (void)frame;
    extern void log_read(char *buf, int len, int *out_len);
    extern int log_get_len(void);

    if (type == 3) {  // Return buffer size
        return log_get_len();
    } else if (type == 4) {  // Read buffer
        int out_len = 0;
        log_read(buf, len, &out_len);
        return out_len;
    }
    return -1;
}

int64_t sys_spawn(void (*entry)(void), uint64_t flags, struct interrupt_frame *frame) {
    (void)frame;
    task_struct_t *task = task_create(entry, flags);
    if (!task) return -1;
    return (int64_t)task->pid;
}

/* -------------------------------------------------------------------------
 * File descriptor helpers
 * ---------------------------------------------------------------------- */

/* Allocate the next free file descriptor in current->files */
static int alloc_fd(void) {
    if (!current || !current->files) return -1;
    for (int i = 3; i < 256; i++) {   /* 0=stdin, 1=stdout, 2=stderr reserved */
        if (!current->files->fd_array[i])
            return i;
    }
    return -1;
}

static file_t *alloc_file(vfs_node_t *node, int flags) {
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (!f) return NULL;
    f->node = node;
    f->offset = 0;
    f->flags = flags;
    f->refcount = 1;
    return f;
}

static void free_file(file_t *f) {
    if (!f) return;
    f->refcount--;
    if (f->refcount <= 0) {
        vfs_close(f->node);
        kfree(f);
    }
}

/* -------------------------------------------------------------------------
 * VFS-backed syscalls
 * ---------------------------------------------------------------------- */

int64_t sys_open(const char *pathname, int64_t flags, int64_t mode, struct interrupt_frame *frame) {
    (void)frame;
    if (!pathname) return -EINVAL;

    int acc_mode = flags & O_ACCMODE;
    if (acc_mode != O_RDONLY && acc_mode != O_WRONLY && acc_mode != O_RDWR) {
        return -EINVAL;
    }

    vfs_node_t *node = vfs_resolve(pathname);
    
    if (!node) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        
        char path_copy[256];
        int i = 0;
        for (; pathname[i] && i < 255; i++) path_copy[i] = pathname[i];
        path_copy[i] = '\0';
        
        char *last_slash = path_copy;
        for (char *p = path_copy; *p; p++) {
            if (*p == '/') last_slash = p;
        }
        
        if (last_slash == path_copy && *last_slash == '/') {
            last_slash++;
        }
        
        *last_slash = '\0';
        vfs_node_t *parent = vfs_resolve(path_copy);
        if (!parent || !(parent->flags & VFS_DIRECTORY)) {
            return -ENOENT;
        }
        
        if (parent->finddir) {
            vfs_node_t *existing = parent->finddir(parent, last_slash + 1);
            if (existing && (flags & O_EXCL)) {
                return -EEXIST;
            }
        }
        
        return -ENOSYS;
    }

    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }

    if (node->flags & VFS_DIRECTORY) {
        if (acc_mode != O_RDONLY) {
            return -EISDIR;
        }
    }

    int fd = alloc_fd();
    if (fd < 0) return -EMFILE;

    file_t *f = alloc_file(node, (int)flags);
    if (!f) return -ENOMEM;
    
    if (flags & O_TRUNC && (acc_mode == O_WRONLY || acc_mode == O_RDWR)) {
        if (node->write && !(node->flags & VFS_PIPE)) {
            node->length = 0;
        }
    }
    
    if (flags & O_APPEND) {
        f->offset = node->length;
    }
    
    vfs_open(node);
    current->files->fd_array[fd] = f;
    return (int64_t)fd;
}

int64_t sys_close(int64_t fd, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files) return -1;
    if (fd < 0 || fd >= 256) return -1;
    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f) return -1;
    free_file(f);
    current->files->fd_array[fd] = NULL;
    return 0;
}

int64_t sys_read(int64_t fd, void *buf, int64_t count, struct interrupt_frame *frame) {
    (void)frame;
    if (!buf || count <= 0) return -EINVAL;
    
    /* stdin - use TTY line discipline */
    if (fd == 0) {
        if (console_tty) {
            return tty_read(console_tty, (char *)buf, (int)count);
        }
        /* Fallback to keyboard buffer if TTY not initialized */
        char *char_buf = (char *)buf;
        int64_t read_bytes = 0;
        while (read_bytes < count) {
            extern wait_queue_head_t kbd_wait;
            extern char keyboard_pop_char(void);
            
            char c = keyboard_pop_char();
            if (c == 0) {
                if (read_bytes > 0) {
                    break;
                }
                wait_event(kbd_wait, (c = keyboard_pop_char()) != 0);
            }
            char_buf[read_bytes++] = c;
            if (c == '\n') {
                break;
            }
        }
        return read_bytes;
    }

    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;
    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f) return -EBADF;
    uint32_t ret = vfs_read(f->node, (uint32_t)f->offset, (uint32_t)count, (uint8_t *)buf);
    f->offset += ret;
    return (int64_t)ret;
}

int64_t sys_write(int64_t fd, const void *buf, int64_t count, struct interrupt_frame *frame) {
    (void)frame;
    if (!buf || count <= 0) return -EINVAL;

    /* stdout/stderr - use TTY */
    if (fd == 1 || fd == 2) {
        if (console_tty) {
            return tty_write(console_tty, (const char *)buf, (int)count);
        }
        /* Fallback to screen_putc if TTY not initialized */
        const char *str = (const char *)buf;
        for (int64_t i = 0; i < count; i++)
            screen_putc(str[i]);
        return count;
    }

    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;
    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f) return -EBADF;
    uint32_t ret = vfs_write(f->node, (uint32_t)f->offset, (uint32_t)count, (const uint8_t *)buf);
    f->offset += ret;
    return (int64_t)ret;
}

int64_t sys_execve(const char *path, const char **argv, const char **envp, struct interrupt_frame *frame) {
    (void)frame;
    if (!path) return -1;

    vfs_node_t *node = vfs_resolve(path);
    if (!node || !(node->flags & VFS_FILE)) return -1;

    uint32_t file_size = node->length;
    if (!file_size) return -1;

    uint8_t *elf_data = (uint8_t *)kmalloc(file_size);
    if (!elf_data) return -1;

    vfs_read(node, 0, file_size, elf_data);

    /* Create a fresh new PML4 for the new program */
    uint64_t new_pml4 = vmm_create_pml4();
    if (!new_pml4) {
        kfree(elf_data);
        return -1;
    }

    /* Load ELF into the new PML4 */
    uint64_t entry = elf_load(new_pml4, elf_data, file_size);
    kfree(elf_data);

    if (!entry) {
        vmm_free_pml4(new_pml4);
        return -1;
    }

    /* Allocate user stack */
    uint64_t user_stack_top = 0x7FFFFFF000ULL;
    uint64_t stack_phys = pmm_alloc_block();
    if (!stack_phys) {
        vmm_free_pml4(new_pml4);
        return -1;
    }
    
    /* Map stack in the new PML4 */
    vmm_map_page_in_pml4(new_pml4, user_stack_top - 4096, stack_phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);

    /* Free old PML4 and switch to the new one */
    extern uint64_t kernel_pml4_phys;
    if (current->mm) {
        if (current->mm->pml4_phys != 0 && current->mm->pml4_phys != kernel_pml4_phys) {
            vmm_free_pml4(current->mm->pml4_phys);
        }
        current->mm->pml4_phys = new_pml4;
        current->mm->start_stack = user_stack_top;
    }

    /* Switch CR3 to the new page directory */
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_pml4) : "memory");

    /* Map stack page into current address space to build argv/envp */
    vmm_map_page_in_pml4(new_pml4, user_stack_top - 4096, stack_phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);

    /* Build user stack with argv/envp */
    uint64_t sp = user_stack_top;
    
    /* Count argv and envp */
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    int envc = 0;
    if (envp) {
        while (envp[envc]) envc++;
    }

    /* Calculate space needed for strings */
    int total_str_len = 0;
    if (argv) {
        for (int i = 0; i < argc; i++) {
            const char *s = argv[i];
            if (s) {
                while (*s++) total_str_len++;
            }
        }
    }
    if (envp) {
        for (int i = 0; i < envc; i++) {
            const char *s = envp[i];
            if (s) {
                while (*s++) total_str_len++;
            }
        }
    }
    
    /* Reserve space for strings + argv/envp pointers + argc + alignment */
    uint64_t str_space = (total_str_len + 15) & ~15;
    uint64_t ptr_space = (argc + 1 + envc + 1 + 1) * 8;  // argv + NULL + envp + NULL + argc
    sp -= str_space + ptr_space + 16;
    sp &= ~15ULL;  // 16-byte align

    uint64_t *stack_argv = (uint64_t *)sp;
    uint64_t *stack_envp = stack_argv + argc + 1;
    char *str_base = (char *)(stack_envp + envc + 1);
    char *str_ptr = str_base;

    /* Copy argv strings */
    if (argv) {
        for (int i = 0; i < argc; i++) {
            if (!argv[i]) { stack_argv[i] = 0; continue; }
            stack_argv[i] = (uint64_t)str_ptr;
            const char *s = argv[i];
            while (*s) *str_ptr++ = *s++;
            *str_ptr++ = '\0';
        }
    }
    stack_argv[argc] = 0;

    /* Copy envp strings */
    if (envp) {
        for (int i = 0; i < envc; i++) {
            if (!envp[i]) { stack_envp[i] = 0; continue; }
            stack_envp[i] = (uint64_t)str_ptr;
            const char *s = envp[i];
            while (*s) *str_ptr++ = *s++;
            *str_ptr++ = '\0';
        }
    }
    stack_envp[envc] = 0;

    /* Push argc at the very bottom of the stack frame */
    uint64_t *stack_argc = stack_envp + envc + 1;
    *stack_argc = argc;

    /* Final user stack pointer: points to argc */
    uint64_t final_sp = (uint64_t)stack_argc;

    /* Jump to Ring 3 */
    tss_set_kernel_stack(current->kernel_stack);
    jump_to_usermode_with_args(entry, final_sp, argc, (uint64_t)stack_argv, (uint64_t)stack_envp);

    return 0; /* unreachable */
}

/* -------------------------------------------------------------------------
 * Memory management (stubs — implemented in backlog)
 * ---------------------------------------------------------------------- */

int64_t sys_brk(void *addr, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->mm) return -1;

    if (current->mm->start_brk == 0) {
        current->mm->start_brk = 0x20000000; /* 512MB Virtual Address Heap */
        current->mm->brk = current->mm->start_brk;
    }

    if (addr == NULL) {
        return (int64_t)current->mm->brk;
    }

    uint64_t new_brk = (uint64_t)addr;
    uint64_t old_brk = current->mm->brk;

    if (new_brk > old_brk) {
        uint64_t start_page = (old_brk + 4095) & ~4095;
        uint64_t end_page = (new_brk + 4095) & ~4095;
        for (uint64_t pg = start_page; pg < end_page; pg += 4096) {
            uint64_t phys = pmm_alloc_block();
            if (!phys) return -1;
            vmm_map_page_in_pml4(current->mm->pml4_phys, pg, phys,
                                 VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
        }
    } else if (new_brk < old_brk) {
        uint64_t start_page = (new_brk + 4095) & ~4095;
        uint64_t end_page = (old_brk + 4095) & ~4095;
        for (uint64_t pg = start_page; pg < end_page; pg += 4096) {
            uint64_t phys = vmm_get_phys(current->mm->pml4_phys, pg);
            if (phys) {
                pmm_free_block(phys);
            }
            vmm_unmap_page_in_pml4(current->mm->pml4_phys, pg);
        }
    }

    current->mm->brk = new_brk;
    return (int64_t)current->mm->brk;
}

void *sys_mmap(void *addr, int64_t length, int64_t prot, int64_t flags, int64_t fd, int64_t offset, struct interrupt_frame *frame) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset; (void)frame;
    if (!current || !current->mm || length <= 0) return (void *)-1;

    static uint64_t mmap_base = 0x30000000; /* 768MB Virtual Address Mmap Area */
    uint64_t virt = mmap_base;
    uint64_t aligned_length = (length + 4095) & ~4095;

    for (uint64_t off = 0; off < aligned_length; off += 4096) {
        uint64_t phys = pmm_alloc_block();
        if (!phys) return (void *)-1;
        vmm_map_page_in_pml4(current->mm->pml4_phys, virt + off, phys,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    mmap_base += aligned_length;
    return (void *)virt;
}

int64_t sys_munmap(void *addr, int64_t length, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->mm) return -1;
    if (!addr || length <= 0) return -1;

    uint64_t start = (uint64_t)addr;
    uint64_t end = start + length;
    
    /* Align to page boundaries */
    start &= ~4095ULL;
    end = (end + 4095) & ~4095ULL;
    
    if (start >= end) return -1;
    
    uint64_t pml4_phys = current->mm->pml4_phys;
    if (!pml4_phys) return -1;
    
    for (uint64_t v = start; v < end; v += 4096) {
        uint64_t phys = vmm_get_phys(pml4_phys, v);
        if (phys) {
            pmm_free_block(phys);
        }
        vmm_unmap_page_in_pml4(pml4_phys, v);
    }
    
    return 0;
}

int64_t sys_ioctl(int64_t fd, uint64_t request, void *arg, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;
    
    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f) return -EBADF;
    
    vfs_node_t *node = f->node;
    
    /* Handle TTY ioctls for stdin/stdout/stderr */
    if (fd <= 2 && console_tty) {
        return tty_ioctl(console_tty, request, arg);
    }
    
    /* For other files, check if they have an ioctl handler */
    if (node && node->ptr) {
        /* TODO: add ioctl to vfs_node_t */
    }
    
    return -ENOTTY;
}

int64_t sys_pipe(int64_t pipefd[2], struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files || !pipefd) return -1;

    vfs_node_t *write_end;
    vfs_node_t *read_end = pipe_create(&write_end);
    if (!read_end || !write_end) return -1;

    int fd_read = alloc_fd();
    if (fd_read < 0) {
        kfree(read_end);
        kfree(write_end);
        return -1;
    }

    int fd_write = alloc_fd();
    if (fd_write < 0) {
        current->files->fd_array[fd_read] = NULL;
        kfree(read_end);
        kfree(write_end);
        return -1;
    }

    read_end->open(read_end);
    write_end->open(write_end);

    file_t *read_file = (file_t *)kmalloc(sizeof(file_t));
    file_t *write_file = (file_t *)kmalloc(sizeof(file_t));
    if (!read_file || !write_file) {
        if (read_file) kfree(read_file);
        if (write_file) kfree(write_file);
        return -1;
    }
    read_file->node = read_end;
    read_file->offset = 0;
    read_file->flags = 0;
    read_file->refcount = 1;
    write_file->node = write_end;
    write_file->offset = 0;
    write_file->flags = 0;
    write_file->refcount = 1;

    current->files->fd_array[fd_read] = read_file;
    current->files->fd_array[fd_write] = write_file;

    int *user_pipefd = (int *)pipefd;
    user_pipefd[0] = fd_read;
    user_pipefd[1] = fd_write;

    return 0;
}

/* -------------------------------------------------------------------------
 * task_create_user — spawn a new Ring-3 task from an ELF in RamFS
 * ---------------------------------------------------------------------- */

int64_t task_create_user(const char *path) {
    if (!path) return -1;

    vfs_node_t *node = vfs_resolve(path);
    if (!node || !(node->flags & VFS_FILE)) {
        screen_log("FAIL", COLOR_LIGHT_RED, "task_create_user: arquivo nao encontrado");
        return -1;
    }

    uint32_t file_size = node->length;
    if (!file_size) return -1;

    uint8_t *elf_data = (uint8_t *)kmalloc(file_size);
    if (!elf_data) return -1;
    vfs_read(node, 0, file_size, elf_data);

    /* Create a fresh new PML4 */
    uint64_t new_pml4 = vmm_create_pml4();
    if (!new_pml4) {
        kfree(elf_data);
        return -1;
    }

    /* Load ELF into the new PML4 */
    uint64_t entry = elf_load(new_pml4, elf_data, file_size);
    kfree(elf_data);
    if (!entry) {
        vmm_free_pml4(new_pml4);
        return -1;
    }

    /* Allocate user stack */
    uint64_t user_stack_top = 0x7FFFFFF000ULL;
    uint64_t stack_phys = pmm_alloc_block();
    if (!stack_phys) {
        vmm_free_pml4(new_pml4);
        return -1;
    }
    
    /* Map stack in the new PML4 */
    vmm_map_page_in_pml4(new_pml4, user_stack_top - 4096, stack_phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);

    /* Swap PML4 on current task */
    extern uint64_t kernel_pml4_phys;
    if (current->mm) {
        if (current->mm->pml4_phys != 0 && current->mm->pml4_phys != kernel_pml4_phys) {
            vmm_free_pml4(current->mm->pml4_phys);
        }
        current->mm->pml4_phys = new_pml4;
        current->mm->start_stack = user_stack_top;
    }

    /* Load new CR3 */
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_pml4) : "memory");

    /* Jump to Ring 3 */
    tss_set_kernel_stack(current->kernel_stack);
    jump_to_usermode(entry, user_stack_top);

    return 0;
}

/* -------------------------------------------------------------------------
 * lseek — reposition file offset
 * ---------------------------------------------------------------------- */

int64_t sys_lseek(int64_t fd, int64_t offset, int64_t whence, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files) return -1;
    if (fd < 0 || fd >= 256) return -1;
    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f) return -1;
    
    uint64_t new_offset;
    switch (whence) {
        case 0: /* SEEK_SET */
            new_offset = (uint64_t)offset;
            break;
        case 1: /* SEEK_CUR */
            new_offset = f->offset + (uint64_t)offset;
            break;
        case 2: /* SEEK_END */
            new_offset = f->node->length + (uint64_t)offset;
            break;
        default:
            return -1;
    }
    
    if (new_offset > 0xFFFFFFFF) return -1;
    f->offset = (uint32_t)new_offset;
    return (int64_t)f->offset;
}

/* -------------------------------------------------------------------------
 * stat — get file status
 * ---------------------------------------------------------------------- */

int64_t sys_stat(const char *pathname, struct stat *statbuf, struct interrupt_frame *frame) {    (void)frame;
    if (!pathname || !statbuf) return -1;
    
    vfs_node_t *node = vfs_resolve(pathname);
    if (!node) return -1;
    
    statbuf->st_dev = 0;
    statbuf->st_ino = node->inode;
    if (node->flags & VFS_DIRECTORY) {
        statbuf->st_mode = 0x4000; /* S_IFDIR */
    } else if (node->flags & VFS_CHARDEVICE) {
        statbuf->st_mode = 0x2000; /* S_IFCHR */
    } else if (node->flags & VFS_BLOCKDEVICE) {
        statbuf->st_mode = 0x6000; /* S_IFBLK */
    } else {
        statbuf->st_mode = 0x8000; /* S_IFREG */
    }
    statbuf->st_nlink = 1;
    statbuf->st_uid = node->uid;
    statbuf->st_gid = node->gid;
    statbuf->st_rdev = 0;
    statbuf->st_size = node->length;
    statbuf->st_blksize = 512;
    statbuf->st_blocks = (node->length + 511) / 512;
    statbuf->st_atime = 0;
    statbuf->st_mtime = 0;
    statbuf->st_ctime = 0;
    
    return 0;
}

/* -------------------------------------------------------------------------
 * fsync — flush cached file data to disk
 * ---------------------------------------------------------------------- */

int64_t sys_fsync(int64_t fd, struct interrupt_frame *frame) {
    (void)frame;
    if (!current || !current->files) return -1;
    if (fd < 0 || fd >= 256) return -1;
    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f) return -1;

    if (ata_sync() != 0) return -1;

    return 0;
}

/* -------------------------------------------------------------------------
 * Directory syscalls
 * ---------------------------------------------------------------------- */

static vfs_node_t *resolve_parent_and_name(const char *pathname, char *name_out)
{
    if (!pathname) return NULL;

    char path_copy[512];
    int i = 0;
    for (; pathname[i] && i < 511; i++) path_copy[i] = pathname[i];
    path_copy[i] = '\0';

    char *last_slash = NULL;
    for (char *p = path_copy; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        if (name_out) str_copy(name_out, path_copy, VFS_MAX_NAME);
        return current ? current->cwd : vfs_root;
    }

    if (last_slash == path_copy) {
        *last_slash = '\0';
        if (name_out) str_copy(name_out, last_slash + 1, VFS_MAX_NAME);
        return vfs_root;
    }

    *last_slash = '\0';
    if (name_out) str_copy(name_out, last_slash + 1, VFS_MAX_NAME);

    return vfs_resolve(path_copy);
}

int64_t sys_mkdir(const char *pathname, int64_t mode, struct interrupt_frame *frame)
{
    (void)frame;
    if (!pathname) return -EINVAL;

    char name[VFS_MAX_NAME];
    vfs_node_t *parent = resolve_parent_and_name(pathname, name);
    if (!parent || !(parent->flags & VFS_DIRECTORY)) return -ENOENT;

    if (parent->finddir && parent->finddir(parent, name)) return -EEXIST;

    return vfs_mkdir(parent, name, (uint32_t)mode);
}

int64_t sys_rmdir(const char *pathname, struct interrupt_frame *frame)
{
    (void)frame;
    if (!pathname) return -EINVAL;

    char name[VFS_MAX_NAME];
    vfs_node_t *parent = resolve_parent_and_name(pathname, name);
    if (!parent || !(parent->flags & VFS_DIRECTORY)) return -ENOENT;

    return vfs_rmdir(parent, name);
}

int64_t sys_unlink(const char *pathname, struct interrupt_frame *frame)
{
    (void)frame;
    if (!pathname) return -EINVAL;

    char name[VFS_MAX_NAME];
    vfs_node_t *parent = resolve_parent_and_name(pathname, name);
    if (!parent || !(parent->flags & VFS_DIRECTORY)) return -ENOENT;

    return vfs_unlink(parent, name);
}

int64_t sys_chdir(const char *pathname, struct interrupt_frame *frame)
{
    (void)frame;
    if (!pathname || !current) return -EINVAL;

    vfs_node_t *node = vfs_resolve(pathname);
    if (!node) return -ENOENT;
    if (!(node->flags & VFS_DIRECTORY)) return -ENOTDIR;

    current->cwd = node;
    return 0;
}

int64_t sys_fchdir(int64_t fd, struct interrupt_frame *frame)
{
    (void)frame;
    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;

    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f || !(f->node->flags & VFS_DIRECTORY)) return -ENOTDIR;

    current->cwd = f->node;
    return 0;
}

int64_t sys_getcwd(char *buf, int64_t size, struct interrupt_frame *frame)
{
    (void)frame;
    if (!current || !buf || size <= 0) return -EINVAL;

    if (size < 2) return -ERANGE;
    buf[0] = '/';
    buf[1] = '\0';
    return 0;
}

int64_t sys_rename(const char *oldpath, const char *newpath, struct interrupt_frame *frame)
{
    (void)frame;
    if (!oldpath || !newpath) return -EINVAL;

    char old_name[VFS_MAX_NAME];
    char new_name[VFS_MAX_NAME];
    vfs_node_t *old_parent = resolve_parent_and_name(oldpath, old_name);
    vfs_node_t *new_parent = resolve_parent_and_name(newpath, new_name);

    if (!old_parent || !new_parent ||
        !(old_parent->flags & VFS_DIRECTORY) ||
        !(new_parent->flags & VFS_DIRECTORY))
        return -ENOENT;

    return vfs_rename(old_parent, old_name, new_parent, new_name);
}

int64_t sys_getdents(int64_t fd, struct dirent *dirp, int64_t count, struct interrupt_frame *frame)
{
    (void)frame;
    if (!current || !current->files) return -EBADF;
    if (fd < 0 || fd >= 256) return -EBADF;
    if (!dirp || count <= 0) return -EINVAL;

    file_t *f = (file_t *)current->files->fd_array[fd];
    if (!f || !(f->node->flags & VFS_DIRECTORY)) return -ENOTDIR;

    vfs_node_t *node = f->node;
    int written = 0;
    int entries = 0;

    while (entries < count) {
        vfs_dirent_t *vde = vfs_readdir(node, entries);
        if (!vde) break;

        struct dirent *de = (struct dirent *)((char *)dirp + written);
        de->d_ino = vde->inode;
        de->d_off = entries;
        de->d_reclen = sizeof(struct dirent);
        de->d_type = 0;
        str_copy(de->d_name, vde->name, 256);

        written += sizeof(struct dirent);
        entries++;
    }

    return written;
}

#include <net/net.h>
#include <string.h>

int64_t sys_socket(int domain, int type, int protocol, struct interrupt_frame *frame) {
    (void)frame;
    if (domain != AF_INET) return -1;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -1;
    
    struct udp_sock *sock = udp_socket(domain, type, protocol);
    if (!sock) return -1;
    
    int fd = alloc_fd();
    if (fd < 0) {
        udp_close(sock);
        return -1;
    }
    
    current->files->fd_array[fd] = (void *)sock;
    return fd;
}

int64_t sys_bind(int sockfd, const struct sockaddr *addr, int addrlen, struct interrupt_frame *frame) {
    (void)frame;
    if (sockfd < 0 || sockfd >= 256) return -1;
    if (!addr || addrlen < sizeof(struct sockaddr_in)) return -1;
    
    struct udp_sock *sock = (struct udp_sock *)current->files->fd_array[sockfd];
    if (!sock) return -1;
    
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    if (sin->sin_family != AF_INET) return -1;
    
    sock->dev = netif_default;
    return udp_bind(sock, (const uint8_t *)&sin->sin_addr, ntohs(sin->sin_port));
}

int64_t sys_sendto(int sockfd, const void *buf, int len, int flags, const struct sockaddr *dest_addr, int addrlen, struct interrupt_frame *frame) {
    (void)frame;
    (void)flags;
    if (sockfd < 0 || sockfd >= 256) return -1;
    if (!buf || len <= 0) return -1;
    if (!dest_addr || addrlen < sizeof(struct sockaddr_in)) return -1;
    
    struct udp_sock *sock = (struct udp_sock *)current->files->fd_array[sockfd];
    if (!sock) return -1;
    
    struct sockaddr_in *sin = (struct sockaddr_in *)dest_addr;
    if (sin->sin_family != AF_INET) return -1;
    
    return udp_sendto(sock, buf, len, (const uint8_t *)&sin->sin_addr, ntohs(sin->sin_port));
}

int64_t sys_recvfrom(int sockfd, void *buf, int len, int flags, struct sockaddr *src_addr, int *addrlen, struct interrupt_frame *frame) {
    (void)frame;
    (void)flags;
    if (sockfd < 0 || sockfd >= 256) return -1;
    if (!buf || len <= 0) return -1;
    
    struct udp_sock *sock = (struct udp_sock *)current->files->fd_array[sockfd];
    if (!sock) return -1;
    
    uint8_t src_ip[4];
    uint16_t src_port;
    int ret = udp_recvfrom(sock, buf, len, src_ip, &src_port);
    
    if (ret > 0 && src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(src_port);
        sin->sin_addr = *(uint32_t *)src_ip;
        memset(sin->sin_zero, 0, 8);
        *addrlen = sizeof(struct sockaddr_in);
    }
    
    return ret;
}

int64_t sys_sock_close(int64_t fd, struct interrupt_frame *frame) {
    (void)frame;
    if (fd < 0 || fd >= 256) return -1;
    if (!current || !current->files) return -1;
    
    void *f = current->files->fd_array[fd];
    if (!f) return -1;
    
    struct udp_sock *sock = (struct udp_sock *)f;
    if (sock->bound) {
        udp_close(sock);
    }
    
    current->files->fd_array[fd] = NULL;
    return 0;
}
