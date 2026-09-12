#include <syscall.h>
#include <task.h>
#include <screen.h>
#include <kheap.h>
#include <vmm.h>
#include <pmm.h>
#include <timer.h>
#include <dos/dos.h>
#include <dos/path.h>
#include <exec/exec.h>
#include <elf.h>
#include <gdt.h>
#include <keyboard.h>
#include <rtc.h>
#include <panic.h>
#include <idt.h>
#include <ata.h>
#include <tty.h>
#include <string.h>
#include <bsdsocket.h>
#include <uaccess.h>

/* ------------------------------------------------------------------ */
/* Dispatch table                                                       */
/* ------------------------------------------------------------------ */

static int64_t (*syscall_table[NR_SYSCALLS])(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, struct interrupt_frame *) = {0};

void syscall_init(void) {
    syscall_table[AOS_Exit]         = (void *)aos_exit;
    syscall_table[AOS_SpawnTask]    = (void *)aos_spawn_task;
    syscall_table[AOS_Read]         = (void *)aos_read;
    syscall_table[AOS_Write]        = (void *)aos_write;
    syscall_table[AOS_Open]         = (void *)aos_open;
    syscall_table[AOS_Close]        = (void *)aos_close;
    syscall_table[AOS_Wait]         = (void *)aos_wait;
    syscall_table[AOS_LoadSeg]      = (void *)aos_loadseg;
    syscall_table[AOS_SetBrk]       = (void *)aos_setbrk;
    syscall_table[AOS_AllocMem]     = (void *)aos_allocmem;
    syscall_table[AOS_FreeMem]      = (void *)aos_freemem;
    syscall_table[AOS_CreatePool]   = (void *)aos_create_pool;
    syscall_table[AOS_DeletePool]   = (void *)aos_delete_pool;
    syscall_table[AOS_AllocPooled]  = (void *)aos_alloc_pooled;
    syscall_table[AOS_FreePooled]   = (void *)aos_free_pooled;
    syscall_table[AOS_PoolAvail]    = (void *)aos_pool_avail;
    syscall_table[AOS_DoIO]         = (void *)aos_doio;
    syscall_table[AOS_FindTask]     = (void *)aos_find_task;
    syscall_table[AOS_Yield]        = (void *)aos_yield;
    syscall_table[AOS_Delay]        = (void *)aos_delay;
    syscall_table[AOS_GetSysTime]   = (void *)aos_getsystime;
    syscall_table[AOS_Clock]        = (void *)aos_clock;
    syscall_table[AOS_PutStr]       = (void *)aos_putstr;
    syscall_table[AOS_IoErr]        = (void *)aos_ioerr;
    syscall_table[AOS_SetIoErr]     = (void *)aos_set_ioerr;
    syscall_table[AOS_AddTask]      = (void *)aos_addtask;
    syscall_table[AOS_Signal]       = (void *)aos_signal;
    syscall_table[AOS_SetSignal]    = (void *)aos_setsignal;
    syscall_table[AOS_ReturnSignal] = (void *)aos_return_signal;
    syscall_table[AOS_SendSignal]   = (void *)aos_send_signal;
    syscall_table[AOS_Pipe]         = (void *)aos_pipe;
    syscall_table[AOS_Seek]         = (void *)aos_seek;
    syscall_table[AOS_Examine]      = (void *)aos_examine;
    syscall_table[AOS_ExamineDir]   = (void *)aos_examine_dir;
    syscall_table[AOS_Flush]        = (void *)aos_flush;
    syscall_table[AOS_CreateDir]    = (void *)aos_create_dir;
    syscall_table[AOS_DeleteDir]    = (void *)aos_delete_dir;
    syscall_table[AOS_DeleteFile]   = (void *)aos_delete_file;
    syscall_table[AOS_CurrentDir]   = (void *)aos_current_dir;
    syscall_table[AOS_CurrentDirFD] = (void *)aos_current_dir_fd;
    syscall_table[AOS_LockCWD]      = (void *)aos_lock_cwd;
    syscall_table[AOS_Rename]       = (void *)aos_rename;
    syscall_table[AOS_Socket]       = (void *)aos_socket;
    syscall_table[AOS_Bind]         = (void *)aos_bind;
    syscall_table[AOS_Send]         = (void *)aos_send;
    syscall_table[AOS_Recv]         = (void *)aos_recv;
    syscall_table[AOS_CloseSocket]  = (void *)aos_close_socket;
    syscall_table[AOS_Assign]       = (void *)aos_assign;
    syscall_table[AOS_CreatePort]   = (void *)aos_create_port;
    syscall_table[AOS_DeletePort]   = (void *)aos_delete_port;
    syscall_table[AOS_PutMsg]       = (void *)aos_put_msg;
    syscall_table[AOS_GetMsg]       = (void *)aos_get_msg;
    syscall_table[AOS_WaitPort]     = (void *)aos_wait_port;
    syscall_table[AOS_ReplyMsg]     = (void *)aos_reply_msg;
    /* bsdsocket.library */
    syscall_table[AOS_Select]       = (void *)aos_select;
    syscall_table[AOS_SetSockOpt]   = (void *)aos_setsockopt;
    syscall_table[AOS_GetSockOpt]   = (void *)aos_getsockopt;
    syscall_table[AOS_GetSocketAddr]= (void *)aos_get_socket_addr;
    syscall_table[AOS_SocketIOCtl]  = (void *)aos_socketioctl;
    syscall_table[AOS_SocketBaseTags]= (void *)aos_socket_base_tags;
    syscall_table[AOS_SendTo]       = (void *)aos_sendto;
    syscall_table[AOS_RecvFrom]     = (void *)aos_recvfrom;
}

void syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, struct interrupt_frame *frame) {
    if (num >= NR_SYSCALLS || !syscall_table[num]) {
        if (current) current->errno_val = AOS_ERR_NOT_FOUND;
        current->rax = (uint64_t)(-AOS_ERR_NOT_FOUND);
        return;
    }
    int64_t ret = syscall_table[num](a1, a2, a3, a4, a5, a6, frame);
    current->rax = (uint64_t)ret;
}

/* ================================================================== */
/* Task management                                                      */
/* ================================================================== */

int64_t aos_exit(int64_t code, struct interrupt_frame *frame) {
    (void)frame;
    task_exit((int)code);
    return 0;
}

/* AOS_SpawnTask — CreateTask replacement.
 * a1 = entry address, a2 = stack size.
 * No COW fork — AmigaOS creates independent tasks. */
int64_t aos_spawn_task(int64_t entry_addr, int64_t stack_size, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_BAD_ARGUMENT;
    if (entry_addr == 0) return -AOS_ERR_BAD_ARGUMENT;
    if (stack_size <= 0) stack_size = 65536; /* default 64K */

    task_struct_t *child = task_create((void (*)(void))entry_addr, 0);
    if (!child) return -AOS_ERR_NO_MEMORY;

    return (int64_t)child->pid;
}

int64_t aos_find_task(struct interrupt_frame *frame) {
    (void)frame;
    return current ? (int64_t)current->pid : 0;
}

int64_t aos_addtask(void (*entry)(void), uint64_t flags, struct interrupt_frame *frame) {
    (void)frame;
    task_struct_t *task = task_create(entry, flags);
    if (!task) return -AOS_ERR_NO_MEMORY;
    return (int64_t)task->pid;
}

int64_t aos_yield(struct interrupt_frame *frame) {
    (void)frame;
    extern volatile uint64_t need_resched;
    need_resched = 1;
    return 0;
}

/* ================================================================== */
/* Wait — simplified task exit collection (no POSIX WIFEXITED)          */
/* ================================================================== */

/* Forward declaration for timer callback */
static void wait_timeout_callback(uint64_t data);

/* ================================================================== */
/* Wait — signal bitmask wait with optional timeout                    */
/* ================================================================== */

int64_t aos_wait(int64_t signal_bits, int64_t timeout_ms, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;

    /* If signal_bits == 0, just yield */
    if (signal_bits == 0) {
        extern volatile uint64_t need_resched;
        need_resched = 1;
        return 0;
    }

    /* Check if any requested signals are already pending */
    uint32_t ready = (uint32_t)signal_bits & current->sig_recv;
    if (ready)
        return (int64_t)ready;

    /* Set up signal wait mask */
    current->sig_wait = (uint32_t)signal_bits;

    /* Set up timeout timer if requested */
    timer_entry_t timeout_timer;
    bool has_timeout = (timeout_ms > 0);
    if (has_timeout) {
        uint64_t timeout_ticks = (timeout_ms * 100) / 1000;  /* ms to ticks (100Hz) */
        if (timeout_ticks == 0) timeout_ticks = 1;
        
        timeout_timer.expires = timer_get_ticks() + timeout_ticks;
        timeout_timer.function = wait_timeout_callback;
        timeout_timer.data = (uint64_t)current;
        timeout_timer.next = NULL;
        timer_add(&timeout_timer);
    }

    /* Mark task as waiting for signals */
    current->state = TASK_STATE_WAITING;

    /* Double-check for signals that may have arrived while setting up */
    ready = (uint32_t)signal_bits & current->sig_recv;
    if (ready) {
        current->sig_wait = 0;
        current->state = TASK_STATE_RUNNING;
        if (has_timeout) timer_remove(&timeout_timer);
        return (int64_t)ready;
    }

    /* Yield to scheduler — will resume when signal arrives or timeout fires */
    extern volatile uint64_t need_resched;
    need_resched = 1;
    schedule();

    /* Resumed — collect signals that arrived */
    ready = (uint32_t)signal_bits & current->sig_recv;
    current->sig_wait = 0;
    current->state = TASK_STATE_RUNNING;

    if (has_timeout) timer_remove(&timeout_timer);

    return (int64_t)ready;
}

/* Timeout callback — wakes up the waiting task */
static void wait_timeout_callback(uint64_t data) {
    task_struct_t *task = (task_struct_t *)data;
    if (task && task->state == TASK_STATE_WAITING) {
        task->state = TASK_STATE_RUNNING;
        extern volatile uint64_t need_resched;
        need_resched = 1;
    }
}

/* ================================================================== */
/* Memory — ExecAllocMem / ExecFreeMem (replace brk/mmap)              */
/* ================================================================== */

int64_t aos_setbrk(int64_t size, struct interrupt_frame *frame) {
    (void)frame;
    if (size <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (!current || !current->mm) return -AOS_ERR_BAD_ARGUMENT;

    static uint64_t brk_base = 0x20000000;
    uint64_t virt = brk_base;
    uint64_t aligned = ((uint64_t)size + 4095) & ~4095ULL;
    uint64_t end;
    if (__builtin_add_overflow(virt, aligned, &end)) return -AOS_ERR_BAD_ARGUMENT;

    for (uint64_t off = 0; off < aligned; off += 4096) {
        uint64_t phys = pmm_alloc_block();
        if (!phys) {
            for (uint64_t u = 0; u < off; u += 4096) {
                uint64_t p = vmm_get_phys(current->mm->pml4_phys, virt + u);
                if (p) pmm_free_block(p);
                vmm_unmap_page_in_pml4(current->mm->pml4_phys, virt + u);
            }
            return -AOS_ERR_NO_MEMORY;
        }
        if (!vmm_map_page_in_pml4(current->mm->pml4_phys, virt + off, phys,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX)) {
            pmm_free_block(phys);
            for (uint64_t u = 0; u < off; u += 4096) {
                uint64_t p = vmm_get_phys(current->mm->pml4_phys, virt + u);
                if (p) pmm_free_block(p);
                vmm_unmap_page_in_pml4(current->mm->pml4_phys, virt + u);
            }
            return -AOS_ERR_NO_MEMORY;
        }
    }

    vma_add(current->mm, virt, end,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX,
            VMA_TYPE_ANON);
    brk_base = end;
    return (int64_t)virt;
}

int64_t aos_allocmem(int64_t size, int64_t mem_flags, struct interrupt_frame *frame) {
    (void)mem_flags;
    (void)frame;
    if (size <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (!current || !current->mm) return -AOS_ERR_BAD_ARGUMENT;

    static uint64_t mmap_base = 0x30000000;
    uint64_t virt = mmap_base;
    uint64_t aligned = ((uint64_t)size + 4095) & ~4095ULL;
    uint64_t end;
    if (__builtin_add_overflow(virt, aligned, &end)) return -AOS_ERR_BAD_ARGUMENT;

    for (uint64_t off = 0; off < aligned; off += 4096) {
        uint64_t phys = pmm_alloc_block();
        if (!phys) {
            for (uint64_t u = 0; u < off; u += 4096) {
                uint64_t p = vmm_get_phys(current->mm->pml4_phys, virt + u);
                if (p) pmm_free_block(p);
                vmm_unmap_page_in_pml4(current->mm->pml4_phys, virt + u);
            }
            return -AOS_ERR_NO_MEMORY;
        }
        if (!vmm_map_page_in_pml4(current->mm->pml4_phys, virt + off, phys,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX)) {
            pmm_free_block(phys);
            for (uint64_t u = 0; u < off; u += 4096) {
                uint64_t p = vmm_get_phys(current->mm->pml4_phys, virt + u);
                if (p) pmm_free_block(p);
                vmm_unmap_page_in_pml4(current->mm->pml4_phys, virt + u);
            }
            return -AOS_ERR_NO_MEMORY;
        }
    }

    vma_add(current->mm, virt, end,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX,
            VMA_TYPE_ANON);
    mmap_base = end;
    return (int64_t)virt;
}

int64_t aos_freemem(int64_t addr, int64_t size, struct interrupt_frame *frame) {
    (void)frame;
    if (!addr || size <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (!current || !current->mm) return -AOS_ERR_BAD_ARGUMENT;

    uint64_t start = (uint64_t)addr & ~4095ULL;
    uint64_t end = ((uint64_t)addr + (uint64_t)size + 4095) & ~4095ULL;
    uint64_t pml4 = current->mm->pml4_phys;

    for (uint64_t v = start; v < end; v += 4096) {
        uint64_t phys = vmm_get_phys(pml4, v);
        if (phys) pmm_free_block(phys);
        vmm_unmap_page_in_pml4(pml4, v);
    }
    vma_remove(current->mm, start, end);
    return 0;
}

/* ================================================================== */
/* Memory Pools                                                        */
/* ================================================================== */

int64_t aos_create_pool(int64_t flags, int64_t pudge_size, int64_t thresh_size, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;
    
    mem_pool_t *pool = exec_create_pool((uint32_t)flags, (uint32_t)pudge_size, (uint32_t)thresh_size);
    if (!pool) return -AOS_ERR_NO_MEMORY;
    
    return (int64_t)pool;
}

int64_t aos_delete_pool(int64_t pool_ptr, struct interrupt_frame *frame) {
    (void)frame;
    if (!pool_ptr) return -AOS_ERR_BAD_ARGUMENT;
    
    exec_delete_pool((mem_pool_t *)pool_ptr);
    return 0;
}

int64_t aos_alloc_pooled(int64_t pool_ptr, int64_t size, struct interrupt_frame *frame) {
    (void)frame;
    if (!pool_ptr || size <= 0) return -AOS_ERR_BAD_ARGUMENT;
    
    void *ptr = exec_alloc_pooled((mem_pool_t *)pool_ptr, (uint32_t)size);
    if (!ptr) return -AOS_ERR_NO_MEMORY;
    
    return (int64_t)ptr;
}

int64_t aos_free_pooled(int64_t pool_ptr, int64_t ptr, int64_t size, struct interrupt_frame *frame) {
    (void)frame;
    if (!pool_ptr || !ptr || size <= 0) return -AOS_ERR_BAD_ARGUMENT;
    
    exec_free_pooled((mem_pool_t *)pool_ptr, (void *)ptr, (uint32_t)size);
    return 0;
}

int64_t aos_pool_avail(int64_t pool_ptr, int64_t flags, struct interrupt_frame *frame) {
    (void)frame;
    if (!pool_ptr) return -AOS_ERR_BAD_ARGUMENT;
    
    return (int64_t)exec_pool_available((mem_pool_t *)pool_ptr, (uint32_t)flags);
}

/* ================================================================== */
/* Delay — ExecDelay (replaces nanosleep)                              */
/* ================================================================== */

/* ================================================================== */
/* Delay — timer-based (replaces nanosleep)                            */
/* ================================================================== */

static void delay_wakeup(uint64_t task_ptr) {
    task_struct_t *t = (task_struct_t *)task_ptr;
    if (t) t->state = TASK_STATE_RUNNING;
}

int64_t aos_delay(int64_t ticks, struct interrupt_frame *frame) {
    (void)frame;
    if (ticks <= 0) return 0;
    if (!current) return -AOS_ERR_BAD_ARGUMENT;

    timer_entry_t timer;
    timer.expires = timer_get_ticks() + (uint64_t)ticks;
    timer.function = delay_wakeup;
    timer.data = (uint64_t)current;
    current->state = TASK_STATE_UNINTERRUPTIBLE;
    timer_add(&timer);
    schedule();
    return 0;
}

/* ================================================================== */
/* Time — ExecEclock / RTC (replaces clock_gettime)                    */
/* ================================================================== */

int64_t aos_getsystime(aos_timeval_t *tv, struct interrupt_frame *frame) {
    (void)frame;
    if (!tv) return -AOS_ERR_BAD_ARGUMENT;
    if (!rtc_is_initialized()) return -AOS_ERR_NOT_FOUND;

    aos_timeval_t kernel_tv;
    uint64_t ns = rtc_get_epoch_nanoseconds();
    kernel_tv.tv_secs = (int64_t)(ns / 1000000000ULL);
    kernel_tv.tv_micros = (int32_t)((ns % 1000000000ULL) / 1000);
    if (copy_to_user(tv, &kernel_tv, sizeof(kernel_tv)) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return 0;
}

int64_t aos_clock(struct interrupt_frame *frame) {
    (void)frame;
    if (!rtc_is_initialized()) return 0;
    return (int64_t)(rtc_get_epoch_nanoseconds() / 1000000000ULL);
}

/* ================================================================== */
/* Signal — Exec bitmask model (replaces POSIX sigaction)              */
/* ================================================================== */

int64_t aos_signal(int64_t signal_bits, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;
    current->sig_recv |= (uint32_t)signal_bits;
    return 0;
}

int64_t aos_setsignal(int64_t new_mask, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;
    uint32_t old = current->sig_recv;
    current->sig_recv = (uint32_t)new_mask;
    return (int64_t)old;
}

int64_t aos_return_signal(struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;
    return (int64_t)current->sig_recv;
}

int64_t aos_send_signal(int64_t pid, int64_t signal_bits, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;
    task_struct_t *t = task_list;
    if (!t) return -AOS_ERR_NOT_FOUND;
    do {
        if (t->pid == (uint64_t)pid) {
            t->sig_recv |= (uint32_t)signal_bits;
            if (t->state == TASK_STATE_WAITING)
                t->state = TASK_STATE_RUNNING;
            return 0;
        }
        t = t->next;
    } while (t != task_list);
    return -AOS_ERR_NOT_FOUND;
}

/* ================================================================== */
/* Pipe → MsgPort pair (replaces POSIX pipe)                           */
/* ================================================================== */

int64_t aos_pipe(int64_t port_ids[2], struct interrupt_frame *frame) {
    (void)frame;
    if (!port_ids) return -AOS_ERR_BAD_ARGUMENT;

    int32_t p1 = msgport_create("pipe_r");
    int32_t p2 = msgport_create("pipe_w");
    if (p1 < 0 || p2 < 0) {
        if (p1 >= 0) msgport_delete(p1);
        if (p2 >= 0) msgport_delete(p2);
        return -AOS_ERR_NO_MEMORY;
    }

    int64_t kernel_port_ids[2] = { (int64_t)p1, (int64_t)p2 };
    if (copy_to_user(port_ids, kernel_port_ids, sizeof(kernel_port_ids)) != 0) {
        msgport_delete(p1);
        msgport_delete(p2);
        return -AOS_ERR_BAD_ARGUMENT;
    }
    return 0;
}

/* ================================================================== */
/* File I/O — delegate to DOS layer (BPTR handles, not POSIX fd)       */
/* ================================================================== */

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int copy_user_path(char dst[256], const char *src) {
    if (!src || strncpy_from_user(dst, src, 256, NULL) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return 0;
}

int64_t aos_open(const char *name, int64_t mode, struct interrupt_frame *frame) {
    (void)frame;
    if (!name) return -AOS_ERR_BAD_ARGUMENT;

    char kernel_name[256];
    if (strncpy_from_user(kernel_name, name, sizeof(kernel_name), NULL) != 0)
        return -AOS_ERR_BAD_ARGUMENT;

    int32_t dos_mode = MODE_OLDFILE;
    if (mode == AOS_O_WRONLY || mode == (AOS_O_WRONLY | AOS_O_CREAT))
        dos_mode = MODE_NEWFILE;
    else if (mode == AOS_O_RDWR)
        dos_mode = MODE_OLDFILE;

    BPTR fh = dos_open(kernel_name, dos_mode);
    if (!fh) return -(int64_t)dos_io_err();
    return (int64_t)fh;
}

int64_t aos_close(int64_t handle, struct interrupt_frame *frame) {
    (void)frame;
    dos_close((BPTR)handle);
    return 0;
}

int64_t aos_read(int64_t handle, void *buf, int64_t count, struct interrupt_frame *frame) {
    (void)frame;
    if (!buf || count <= 0 || count > INT32_MAX)
        return -AOS_ERR_BAD_ARGUMENT;

    char *kernel_buf = (char *)kmalloc((size_t)count);
    if (!kernel_buf)
        return -AOS_ERR_NO_MEMORY;

    int64_t got;

    /* stdin: use TTY */
    if (handle == 0) {
        if (console_tty)
            got = tty_read(console_tty, kernel_buf, (int)count);
        else {
            got = 0;
            while (got < count) {
                extern wait_queue_head_t kbd_wait;
                extern char keyboard_pop_char(void);
                char c = keyboard_pop_char();
                if (c == 0) {
                    if (got > 0) break;
                    wait_event(kbd_wait, (c = keyboard_pop_char()) != 0);
                }
                kernel_buf[got++] = c;
                if (c == '\n') break;
            }
        }
    } else {
        got = (int64_t)dos_read((BPTR)handle, kernel_buf, (int32_t)count);
    }

    if (got > 0 && copy_to_user(buf, kernel_buf, (size_t)got) != 0) {
        kfree(kernel_buf);
        return -AOS_ERR_BAD_ARGUMENT;
    }
    kfree(kernel_buf);
    return got;
}

int64_t aos_write(int64_t handle, const void *buf, int64_t count, struct interrupt_frame *frame) {
    (void)frame;
    if (!buf || count <= 0 || count > INT32_MAX)
        return -AOS_ERR_BAD_ARGUMENT;

    char *kernel_buf = (char *)kmalloc((size_t)count);
    if (!kernel_buf)
        return -AOS_ERR_NO_MEMORY;
    if (copy_from_user(kernel_buf, buf, (size_t)count) != 0) {
        kfree(kernel_buf);
        return -AOS_ERR_BAD_ARGUMENT;
    }

    int64_t written;

    /* stdout/stderr: use TTY */
    if (handle == 1 || handle == 2) {
        if (console_tty)
            written = tty_write(console_tty, kernel_buf, (int)count);
        else {
            for (int64_t i = 0; i < count; i++)
                screen_putc(kernel_buf[i]);
            written = count;
        }
    } else {
        written = (int64_t)dos_write((BPTR)handle, kernel_buf, (int32_t)count);
    }

    kfree(kernel_buf);
    return written;
}

int64_t aos_seek(int64_t handle, int64_t position, int64_t offset_type, struct interrupt_frame *frame) {
    (void)frame;
    int32_t result = dos_seek((BPTR)handle, (int32_t)position, (int32_t)offset_type);
    return (int64_t)result;
}

int64_t aos_examine(int64_t lock, void *fib_buf, int64_t fib_size, struct interrupt_frame *frame) {
    (void)frame;
    if (!fib_buf || fib_size < (int64_t)sizeof(file_info_block_t))
        return -AOS_ERR_BAD_ARGUMENT;

    file_info_block_t kernel_fib;
    int32_t rc = dos_examine((BPTR)lock, &kernel_fib);
    if (rc == 0 && copy_to_user(fib_buf, &kernel_fib, sizeof(kernel_fib)) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)rc;
}

int64_t aos_examine_dir(int64_t lock, void *fib_buf, int64_t fib_size, struct interrupt_frame *frame) {
    (void)frame;
    if (!fib_buf || fib_size < (int64_t)sizeof(file_info_block_t))
        return -AOS_ERR_BAD_ARGUMENT;

    file_info_block_t kernel_fib;
    int32_t rc = dos_ex_next((BPTR)lock, &kernel_fib);
    if (rc == 0 && copy_to_user(fib_buf, &kernel_fib, sizeof(kernel_fib)) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)rc;
}

int64_t aos_flush(int64_t handle, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)dos_flush((BPTR)handle);
}

/* ================================================================== */
/* DOS directory operations — delegate to DOS layer                     */
/* ================================================================== */

int64_t aos_create_dir(const char *name, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_name[256];
    if (copy_user_path(kernel_name, name) != 0) return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)dos_create_dir(kernel_name);
}

int64_t aos_delete_dir(const char *name, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_name[256];
    if (copy_user_path(kernel_name, name) != 0) return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)dos_delete_file(kernel_name);
}

int64_t aos_delete_file(const char *name, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_name[256];
    if (copy_user_path(kernel_name, name) != 0) return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)dos_delete_file(kernel_name);
}

int64_t aos_current_dir(const char *name, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_name[256];
    if (copy_user_path(kernel_name, name) != 0) return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)dos_current_dir(kernel_name);
}

int64_t aos_current_dir_fd(int64_t handle, struct interrupt_frame *frame) {
    (void)frame;
    char name[256];
    int32_t rc = dos_name_from_lock((BPTR)handle, name, sizeof(name));
    if (rc != 0) return (int64_t)rc;
    return (int64_t)dos_current_dir(name);
}

int64_t aos_lock_cwd(char *buf, int64_t size, struct interrupt_frame *frame) {
    (void)frame;
    if (!buf || size <= 0) return -AOS_ERR_BAD_ARGUMENT;
    char cwd[6];
    str_copy(cwd, "Work:", sizeof(cwd));
    size_t copy_size = (size_t)size < sizeof(cwd) ? (size_t)size : sizeof(cwd);
    if (copy_to_user(buf, cwd, copy_size) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return 0;
}

int64_t aos_rename(const char *old_name, const char *new_name, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_old_name[256];
    char kernel_new_name[256];
    if (copy_user_path(kernel_old_name, old_name) != 0 ||
        copy_user_path(kernel_new_name, new_name) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return (int64_t)dos_rename(kernel_old_name, kernel_new_name);
}

/* ================================================================== */
/* I/O control                                                          */
/* ================================================================== */

int64_t aos_doio(int64_t handle, uint64_t request, void *arg, struct interrupt_frame *frame) {
    (void)frame;
    if (handle <= 2 && console_tty) {
        return tty_ioctl(console_tty, request, arg);
    }
    return dos_do_io((BPTR)handle, request, arg);
}

/* ================================================================== */
/* PutStr — serial/log output                                          */
/* ================================================================== */

int64_t aos_putstr(int type, char *buf, int len, struct interrupt_frame *frame) {
    (void)frame;
    extern void log_read(char *buf, int len, int *out_len);
    extern int log_get_len(void);

    if (type == 3) return log_get_len();
    if (type == 4) {
        int out_len = 0;
        log_read(buf, len, &out_len);
        return out_len;
    }
    return -AOS_ERR_BAD_ARGUMENT;
}

/* ================================================================== */
/* IoErr / SetIoErr                                                     */
/* ================================================================== */

int64_t aos_ioerr(struct interrupt_frame *frame) {
    (void)frame;
    return current ? (int64_t)current->errno_val : 0;
}

int64_t aos_set_ioerr(int64_t err, struct interrupt_frame *frame) {
    (void)frame;
    if (!current) return -AOS_ERR_NOT_FOUND;
    current->errno_val = (int)err;
    return 0;
}

/* ================================================================== */
/* LoadSeg — load ELF into new task                                    */
/* ================================================================== */

int64_t aos_loadseg(const char *path, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_path[256];
    if (copy_user_path(kernel_path, path) != 0)
        return -AOS_ERR_BAD_ARGUMENT;

    /* Open file via DOS layer */
    BPTR fh = dos_open(kernel_path, MODE_OLDFILE);
    if (!fh) return -AOS_ERR_NOT_FOUND;

    file_info_block_t fib;
    dos_examine(fh, &fib);
    dos_close(fh);

    if (fib.fib_DirEntryType > 0) return -AOS_ERR_IS_DIRECTORY;

    /* Re-open for reading */
    fh = dos_open(kernel_path, MODE_OLDFILE);
    if (!fh) return -AOS_ERR_NOT_FOUND;

    uint32_t file_size = (uint32_t)fib.fib_Size;
    if (!file_size) { dos_close(fh); return -AOS_ERR_BAD_ARGUMENT; }

    uint8_t *elf_data = (uint8_t *)kmalloc(file_size);
    if (!elf_data) { dos_close(fh); return -AOS_ERR_NO_MEMORY; }

    dos_read(fh, (void *)elf_data, (int32_t)file_size);
    dos_close(fh);

    /* Create new PML4 */
    uint64_t new_pml4 = vmm_create_pml4();
    if (!new_pml4) { kfree(elf_data); return -AOS_ERR_NO_MEMORY; }

    uint64_t entry = elf_load(new_pml4, elf_data, file_size);
    kfree(elf_data);

    if (!entry) { vmm_free_pml4(new_pml4); return -AOS_ERR_NOT_EXECUTABLE; }

    /* Allocate user stack: 1 MiB window, top page mapped, bottom page guard. */
    uint64_t user_stack_top = 0x7FFFFFF000ULL;
    uint64_t stack_phys = pmm_alloc_block();
    if (!stack_phys) { vmm_free_pml4(new_pml4); return -AOS_ERR_NO_MEMORY; }

    if (!vmm_map_page_in_pml4(new_pml4, user_stack_top - 4096, stack_phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX)) {
        pmm_free_block(stack_phys);
        vmm_free_pml4(new_pml4);
        return -AOS_ERR_NO_MEMORY;
    }

    extern uint64_t kernel_pml4_phys;
    if (current->mm) {
        if (current->mm->pml4_phys != 0 && current->mm->pml4_phys != kernel_pml4_phys) {
            vma_clear(current->mm);
            vmm_free_pml4(current->mm->pml4_phys);
        }
        current->mm->pml4_phys = new_pml4;
        current->mm->start_stack = user_stack_top;
        vma_add(current->mm, user_stack_top - (1024 * 1024), user_stack_top,
                VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX,
                VMA_TYPE_STACK);
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(new_pml4) : "memory");

    tss_set_kernel_stack(current->kernel_stack);
    jump_to_usermode(entry, user_stack_top);

    return 0;
}

/* ================================================================== */
/* bsdsocket.library — AmigaOS-style BSD socket API                     */
/* ================================================================== */

#include <net/net.h>

int64_t aos_socket(int domain, int type, int protocol, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)Socket(domain, type, protocol);
}

int64_t aos_bind(int sockfd, const struct sockaddr *addr, int addrlen, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)Bind(sockfd, addr, addrlen);
}

int64_t aos_send(int sockfd, const void *buf, int len, int flags, const struct sockaddr *dest_addr, int addrlen, struct interrupt_frame *frame) {
    (void)frame; (void)addrlen;
    /* Send() requires connected socket; SendTo() uses dest_addr.
     * We route to SendTo when dest_addr is non-NULL for backward compat. */
    if (dest_addr)
        return (int64_t)SendTo(sockfd, buf, len, flags, dest_addr, addrlen);
    return (int64_t)Send(sockfd, buf, len, flags);
}

int64_t aos_recv(int sockfd, void *buf, int len, int flags, struct sockaddr *src_addr, int *addrlen, struct interrupt_frame *frame) {
    (void)frame;
    if (src_addr && addrlen)
        return (int64_t)RecvFrom(sockfd, buf, len, flags, src_addr, addrlen);
    return (int64_t)Recv(sockfd, buf, len, flags);
}

int64_t aos_close_socket(int64_t fd, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)CloseSocket((int)fd);
}

int64_t aos_select(int64_t width, uint64_t readfds_ptr, uint64_t writefds_ptr,
                   uint64_t exceptfds_ptr, uint64_t timeout_ptr, struct interrupt_frame *frame) {
    (void)frame;
    fd_set *rfds = (fd_set *)readfds_ptr;
    fd_set *wfds = (fd_set *)writefds_ptr;
    fd_set *efds = (fd_set *)exceptfds_ptr;
    struct timeval *tv = (struct timeval *)timeout_ptr;
    return (int64_t)Select((int)width, rfds, wfds, efds, tv);
}

int64_t aos_setsockopt(int64_t sockfd, int64_t level, int64_t optname,
                       uint64_t optval_ptr, int64_t optlen, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)SetSockOpt((int)sockfd, (int)level, (int)optname,
                               (const void *)optval_ptr, (int)optlen);
}

int64_t aos_getsockopt(int64_t sockfd, int64_t level, int64_t optname,
                       uint64_t optval_ptr, uint64_t optlen_ptr, struct interrupt_frame *frame) {
    (void)frame;
    int *optlen = (int *)optlen_ptr;
    return (int64_t)GetSockOpt((int)sockfd, (int)level, (int)optname,
                               (void *)optval_ptr, optlen);
}

int64_t aos_get_socket_addr(int64_t sockfd, uint64_t name_ptr, uint64_t namelen_ptr,
                            struct interrupt_frame *frame) {
    (void)frame;
    int *namelen = (int *)namelen_ptr;
    return (int64_t)GetSocketAddr((int)sockfd, (struct sockaddr *)name_ptr, namelen);
}

int64_t aos_socketioctl(int64_t sockfd, int64_t request, uint64_t arg_ptr,
                        struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)SocketIOCtl((int)sockfd, (int)request, (void *)arg_ptr);
}

int64_t aos_socket_base_tags(uint64_t taglist_ptr, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)SocketBaseTags((struct TagItem *)taglist_ptr);
}

int64_t aos_sendto(int64_t sockfd, uint64_t buf_ptr, int64_t len, int64_t flags,
                   uint64_t to_ptr, int64_t tolen, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)SendTo((int)sockfd, (const void *)buf_ptr, (int)len, (int)flags,
                           (const struct sockaddr *)to_ptr, (int)tolen);
}

int64_t aos_recvfrom(int64_t sockfd, uint64_t buf_ptr, int64_t len, int64_t flags,
                     uint64_t from_ptr, uint64_t fromlen_ptr, struct interrupt_frame *frame) {
    (void)frame;
    int *fromlen = (int *)fromlen_ptr;
    return (int64_t)RecvFrom((int)sockfd, (void *)buf_ptr, (int)len, (int)flags,
                             (struct sockaddr *)from_ptr, fromlen);
}

/* ================================================================== */
/* AmigaOS assigns + message ports                                     */
/* ================================================================== */

#include <assign.h>
#include <msgport.h>

int64_t aos_assign(const char *name, const char *path, int64_t op, struct interrupt_frame *frame) {
    (void)frame;
    if (!name) return -AOS_ERR_BAD_ARGUMENT;
    if (op == ASSIGN_SET)
        return (int64_t)assign_set(name, path);
    if (op == ASSIGN_UNSET)
        return (int64_t)assign_unset(name);
    if (op == ASSIGN_GET) {
        if (!path) return -AOS_ERR_BAD_ARGUMENT;
        char buf[ASSIGN_MAX_PATH];
        if (assign_lookup(name, buf, sizeof buf) != 0)
            return -AOS_ERR_NOT_FOUND;
        char *dst = (char *)path;
        int i = 0;
        while (buf[i] && i < ASSIGN_MAX_PATH - 1) *dst++ = buf[i++];
        *dst = '\0';
        return 0;
    }
    return -AOS_ERR_BAD_ARGUMENT;
}

int64_t aos_create_port(const char *name, struct interrupt_frame *frame) {
    (void)frame;
    char kernel_name[16];
    if (name) {
        if (strncpy_from_user(kernel_name, name, sizeof(kernel_name), NULL) != 0)
            return -AOS_ERR_BAD_ARGUMENT;
        for (int i = 0; kernel_name[i]; ++i) {
            if (kernel_name[i] == ':') {
                kernel_name[i] = '\0';
                break;
            }
        }
        name = kernel_name;
    }
    return msgport_create(name);
}

int64_t aos_delete_port(int64_t id, struct interrupt_frame *frame) {
    (void)frame;
    return msgport_delete((int32_t)id);
}

int64_t aos_put_msg(int64_t id, const msg_t *msg, struct interrupt_frame *frame) {
    (void)frame;
    msg_t kernel_msg;
    if (!msg || copy_from_user(&kernel_msg, msg, sizeof(kernel_msg)) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return msgport_put((int32_t)id, &kernel_msg);
}

int64_t aos_get_msg(int64_t id, msg_t *msg, int64_t *token, struct interrupt_frame *frame) {
    (void)frame;
    if (!msg || !token)
        return -AOS_ERR_BAD_ARGUMENT;

    msg_t kernel_msg;
    int64_t kernel_token = 0;
    int64_t rc = msgport_get((int32_t)id, &kernel_msg, &kernel_token);
    if (rc != MSGPORT_GET_MSG)
        return rc;
    if (copy_to_user(msg, &kernel_msg, sizeof(kernel_msg)) != 0 ||
        copy_to_user(token, &kernel_token, sizeof(kernel_token)) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return rc;
}

int64_t aos_wait_port(int64_t id, int64_t timeout_ms, struct interrupt_frame *frame) {
    (void)frame;
    return msgport_wait((int32_t)id, timeout_ms);
}

int64_t aos_reply_msg(int64_t token, const msg_t *msg, struct interrupt_frame *frame) {
    (void)frame;
    msg_t kernel_msg;
    if (!msg || copy_from_user(&kernel_msg, msg, sizeof(kernel_msg)) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    return msgport_reply(token, &kernel_msg);
}
