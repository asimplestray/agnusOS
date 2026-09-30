#include <syscall.h>
#include <task.h>
#include <screen.h>
#include <serial.h>
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
#include <uapi/drm.h>

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
        if (frame) frame->rax = (uint64_t)(-AOS_ERR_NOT_FOUND);
        if (current) current->rax = (uint64_t)(-AOS_ERR_NOT_FOUND);
        return;
    }
    int64_t ret = syscall_table[num](a1, a2, a3, a4, a5, a6, frame);
    /* Return value travels back in the trap frame: syscall_entry pops GPRs
     * from it, so userspace RAX receives ret (current->rax mirrors it for
     * kernel-side IoErr() consumers). */
    if (frame) frame->rax = (uint64_t)ret;
    if (current) current->rax = (uint64_t)ret;
}

/* ================================================================== */
/* Task management                                                      */
/* ================================================================== */

/* True when the trap came from Ring 3 (user mode). */
static int caller_is_user(struct interrupt_frame *frame) {
    return frame && ((frame->cs & 3) == 3);
}

/* Validate a user-supplied task entry point.
 * Entries inside the canonical user range must be backed by a mapped USER
 * page in the caller's address space. Entries outside it are only accepted
 * from kernel-mm callers (early boot / CPL0-only world) which keep legacy
 * behavior — user tasks must never smuggle a kernel address here. */
static int entry_valid_for_caller(int64_t entry_addr) {
    uint64_t entry;
    if (entry_addr == 0) return 0;
    if (!current || !current->mm) return 0;
    entry = (uint64_t)entry_addr;
    if (entry < VMM_USER_MIN || entry > VMM_USER_MAX) {
        extern uint64_t kernel_pml4_phys;
        return current->mm->pml4_phys == kernel_pml4_phys;
    }
    {
        uint64_t flags = vmm_get_page_flags(current->mm->pml4_phys,
                                            entry & ~0xFFFULL);
        if (!(flags & VMM_FLAG_PRESENT) || !(flags & VMM_FLAG_USER)) return 0;
    }
    return 1;
}

#define SPAWN_STACK_MAX (8ULL * 1024 * 1024)

int64_t aos_exit(int64_t code, struct interrupt_frame *frame) {
    (void)frame;
    task_exit((int)code);
    return 0;
}

/* AOS_SpawnTask — CreateTask replacement.
 * a1 = entry address, a2 = stack size.
 * No COW fork — AmigaOS creates independent tasks.
 *
 * task_create() builds a KERNEL task (CPL0). Ring 3 must not pick its RIP:
 * until user-mode task spawning exists (lifecycle epic), traps from user
 * mode are refused with NO_PERMISSION instead of handing out CPL0. */
int64_t aos_spawn_task(int64_t entry_addr, int64_t stack_size, struct interrupt_frame *frame) {
    if (!current) return -AOS_ERR_BAD_ARGUMENT;
    if (caller_is_user(frame)) return -AOS_ERR_NO_PERMISSION;
    if (!entry_valid_for_caller(entry_addr)) return -AOS_ERR_BAD_ARGUMENT;
    if (stack_size <= 0) stack_size = 65536; /* default 64K */
    if ((uint64_t)stack_size > SPAWN_STACK_MAX) return -AOS_ERR_BAD_ARGUMENT;

    task_struct_t *child = task_create((void (*)(void))entry_addr, 0);
    if (!child) return -AOS_ERR_NO_MEMORY;

    return (int64_t)child->pid;
}

int64_t aos_find_task(struct interrupt_frame *frame) {
    (void)frame;
    return current ? (int64_t)current->pid : 0;
}

int64_t aos_addtask(void (*entry)(void), uint64_t flags, struct interrupt_frame *frame) {
    /* Same policy as SpawnTask: task_create() yields a CPL0 task, so Ring 3
     * callers are refused until user-mode spawning exists. */
    if (!current) return -AOS_ERR_BAD_ARGUMENT;
    if (caller_is_user(frame)) return -AOS_ERR_NO_PERMISSION;
    if (!entry_valid_for_caller((int64_t)entry)) return -AOS_ERR_BAD_ARGUMENT;
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
    timer.next = NULL;
    current->state = TASK_STATE_UNINTERRUPTIBLE;
    timer_add(&timer);
    schedule();
    /* Lifetime: timer lives on our stack. If we woke spuriously before
     * expiry (signal/migration), the entry would otherwise dangle in the
     * timer list. Same pattern as aos_wait: always unlink on return.
     * Harmless if the timer already fired (callback unlinks before run). */
    timer_remove(&timer);
    if (current->state == TASK_STATE_UNINTERRUPTIBLE)
        current->state = TASK_STATE_RUNNING;
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
        return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
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
    int32_t rc = dos_close((BPTR)handle);
    return (int64_t)rc;
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
        return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
    return (int64_t)rc;
}

int64_t aos_examine_dir(int64_t lock, void *fib_buf, int64_t fib_size, struct interrupt_frame *frame) {
    (void)frame;
    if (!fib_buf || fib_size < (int64_t)sizeof(file_info_block_t))
        return -AOS_ERR_BAD_ARGUMENT;

    file_info_block_t kernel_fib;
    int32_t rc = dos_ex_next((BPTR)lock, &kernel_fib);
    if (rc == 0 && copy_to_user(fib_buf, &kernel_fib, sizeof(kernel_fib)) != 0)
        return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
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
        switch ((uint32_t)request) {
        case TCGETS: {
            /* OUT termios: run the driver on a kernel buffer, then copy out. */
            termios_t kt;
            int rc = tty_ioctl(console_tty, request, &kt);
            if (rc == 0) {
                if (!arg || copy_to_user(arg, &kt, sizeof kt) != 0)
                    return -AOS_ERR_BAD_ADDRESS;
            }
            return rc;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            /* IN termios: copy in before the driver sees it. */
            termios_t kt;
            if (!arg || copy_from_user(&kt, arg, sizeof kt) != 0)
                return -AOS_ERR_BAD_ADDRESS;
            return tty_ioctl(console_tty, request, &kt);
        }
        default:
            /* TCFLSH-style value-cast args and unknown requests never
             * dereference arg (ENOTTY) — safe to pass through. */
            return tty_ioctl(console_tty, request, arg);
        }
    }
    /* DRM path: only GEM_CREATE/GEM_MMAP dereference arg (fixed 24B structs);
     * core ioctls ignore it and the rest return ENOTTY. Bounce those two. */
    {
        unsigned int nr = (unsigned int)request;
        if ((nr & 0xff00U) == ((unsigned int)DRM_IOCTL_BASE << 8))
            nr = _IOC_NR(nr);
        if (nr == _IOC_NR(DRM_IOCTL_GEM_CREATE)) {
            struct drm_gem_create kgc;
            int rc;
            if (!arg || copy_from_user(&kgc, arg, sizeof kgc) != 0)
                return -AOS_ERR_BAD_ADDRESS;
            rc = dos_do_io((BPTR)handle, request, &kgc);
            if (rc == 0 && copy_to_user(arg, &kgc, sizeof kgc) != 0)
                return -AOS_ERR_BAD_ADDRESS;
            return rc;
        }
        if (nr == _IOC_NR(DRM_IOCTL_GEM_MMAP)) {
            struct drm_gem_mmap kgm;
            int rc;
            if (!arg || copy_from_user(&kgm, arg, sizeof kgm) != 0)
                return -AOS_ERR_BAD_ADDRESS;
            rc = dos_do_io((BPTR)handle, request, &kgm);
            if (rc == 0 && copy_to_user(arg, &kgm, sizeof kgm) != 0)
                return -AOS_ERR_BAD_ADDRESS;
            return rc;
        }
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
        /* OUT log bytes: read into a kernel buffer, then copy out.
         * Clamped to the ring size (4096); log_read takes the min anyway. */
        int out_len = 0;
        char *kbuf;
        if (!buf || len <= 0) return -AOS_ERR_BAD_ARGUMENT;
        if (len > 4096) len = 4096;
        kbuf = (char *)kmalloc((size_t)len);
        if (!kbuf) return -AOS_ERR_NO_MEMORY;
        log_read(kbuf, len, &out_len);
        if (out_len > 0 && copy_to_user(buf, kbuf, (size_t)out_len) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
        kfree(kbuf);
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
/* Ring-3 spawn — task_create_user (nova task, address space próprio)  */
/* Ao contrário do LoadSeg (que reencarna a task corrente e nunca      */
/* retorna), aqui nasce uma task independente: o shell sobrevive.      */
/* ================================================================== */

#define RING3_STACK_TOP  0x7FFFFFF000ULL
#define RING3_STACK_SIZE (1024ULL * 1024ULL)

/* Primeiro código que uma user task recém-criada executa (via
 * task_trampoline, na sua kernel stack, com current == ela). Ativa o
 * address space próprio e desce para Ring 3. Nunca retorna. */
static void usermode_launch(void) {
    if (!current || !current->mm || !current->mm->pml4_phys ||
        !current->user_entry || !current->user_stack) {
        serial_print("[RING3] usermode_launch sem contexto valido\n");
        task_exit(-AOS_ERR_BAD_ARGUMENT);
    }
    screen_print("[RING3] launch: ativando PML4 e descendo...\n");
    vmm_activate_pml4(current->mm->pml4_phys);
    screen_print("[RING3] launch: iret...\n");
    jump_to_usermode(current->user_entry, current->user_stack);
    __builtin_unreachable();
}

/* Monta mm+PML4+stack e entrega a task ao scheduler. Devolve o PID. */
static int64_t spawn_user_task(uint64_t new_pml4, mm_struct_t *new_mm,
                               uint64_t entry) {
    /* Último portão antes do iret: entry precisa estar numa página USER
     * mapeada (o loader valida o resto; aqui é defesa em profundidade). */
    if (entry < VMM_USER_MIN || entry > VMM_USER_MAX)
        return -AOS_ERR_BAD_ARGUMENT;
    uint64_t fl = vmm_get_page_flags(new_pml4, entry & ~0xFFFULL);
    if (!(fl & VMM_FLAG_PRESENT) || !(fl & VMM_FLAG_USER) ||
        (fl & VMM_FLAG_NX))
        return -AOS_ERR_BAD_ARGUMENT;

    /* cli/sti: a task nasce RUNNING e o timer poderia escaloná-la antes de
     * o mm novo estar anexado (single CPU, timer é o único preemptor). */
    __asm__ volatile("cli" ::: "memory");
    task_struct_t *t = task_create(usermode_launch, 0);
    if (!t) {
        __asm__ volatile("sti" ::: "memory");
        return -AOS_ERR_NO_MEMORY;
    }
    /* task_create herdou o mm corrente: desfaz e anexa o novo. */
    if (t->mm) t->mm->refcount--;
    t->mm = new_mm;
    t->user_entry = entry;
    t->user_stack = RING3_STACK_TOP;
    int64_t pid = (int64_t)t->pid;
    __asm__ volatile("sti" ::: "memory");
    return pid;
}

int64_t task_create_user(const char *path) {
    if (!path || !current) return -AOS_ERR_BAD_ARGUMENT;

    BPTR fh = dos_open(path, MODE_OLDFILE);
    if (!fh) return -AOS_ERR_NOT_FOUND;

    file_info_block_t fib;
    dos_examine(fh, &fib);
    if (fib.fib_DirEntryType > 0) { dos_close(fh); return -AOS_ERR_IS_DIRECTORY; }

    fh = dos_open(path, MODE_OLDFILE);
    if (!fh) return -AOS_ERR_NOT_FOUND;
    uint32_t file_size = (uint32_t)fib.fib_Size;
    if (!file_size) { dos_close(fh); return -AOS_ERR_BAD_ARGUMENT; }

    uint8_t *elf_data = (uint8_t *)kmalloc(file_size);
    if (!elf_data) { dos_close(fh); return -AOS_ERR_NO_MEMORY; }
    dos_read(fh, (void *)elf_data, (int32_t)file_size);
    dos_close(fh);

    mm_struct_t *new_mm = (mm_struct_t *)kmalloc(sizeof(mm_struct_t));
    if (!new_mm) { kfree(elf_data); return -AOS_ERR_NO_MEMORY; }
    new_mm->pml4_phys = vmm_create_pml4();
    if (!new_mm->pml4_phys) { kfree(elf_data); kfree(new_mm); return -AOS_ERR_NO_MEMORY; }
    new_mm->start_stack = RING3_STACK_TOP;
    spinlock_init(&new_mm->lock.lock);
    new_mm->refcount = 1;
    new_mm->vmas = NULL;

    uint64_t entry = elf_load_mm(new_mm->pml4_phys, elf_data, file_size, new_mm);
    kfree(elf_data);
    if (!entry) {
        vmm_free_pml4(new_mm->pml4_phys);
        kfree(new_mm);
        return -AOS_ERR_NOT_EXECUTABLE;
    }

    /* User stack: 1 página no topo + VMA de 1 MiB com guard (igual LoadSeg). */
    uint64_t stack_phys = pmm_alloc_block();
    if (!stack_phys) {
        vmm_free_pml4(new_mm->pml4_phys);
        kfree(new_mm);
        return -AOS_ERR_NO_MEMORY;
    }
    if (!vmm_map_page_in_pml4(new_mm->pml4_phys, RING3_STACK_TOP - 4096, stack_phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX)) {
        pmm_free_block(stack_phys);
        vmm_free_pml4(new_mm->pml4_phys);
        kfree(new_mm);
        return -AOS_ERR_NO_MEMORY;
    }
    vma_add(new_mm, RING3_STACK_TOP - RING3_STACK_SIZE, RING3_STACK_TOP,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX,
            VMA_TYPE_STACK);

    int64_t pid = spawn_user_task(new_mm->pml4_phys, new_mm, entry);
    if (pid < 0) {
        vmm_unmap_page_in_pml4(new_mm->pml4_phys, RING3_STACK_TOP - 4096);
        pmm_free_block(stack_phys);
        vmm_free_pml4(new_mm->pml4_phys);
        kfree(new_mm);
    }
    return pid;
}

/* Selftest de boot: roda /bin/hello em Ring 3 e confere o exit code.
 * O ELF conta falhas das sondas (write válido/inválido, alloc, systime)
 * e sai com esse contador — 0 = tudo certo, sem panic no caminho. */
void ring3_selftest(void) {
    int64_t pid = task_create_user("C:hello");
    if (pid < 0) {
        serial_print("[RING3] FAIL: spawn\n");
        return;
    }

    int code = -99;
    int done = 0;
    uint64_t start = timer_get_ticks();
    uint64_t last_tick = start;
    while (timer_get_ticks() - start < 1000) {
        if (timer_get_ticks() - last_tick > 200) {
            last_tick = timer_get_ticks();
            serial_print("[RING3] aguardando task...\n");
        }
        task_struct_t *t = NULL;
        if (task_list) {
            task_struct_t *it = task_list;
            do {
                if (it->pid == (uint64_t)pid) { t = it; break; }
                it = it->next;
            } while (it != task_list);
        }
        if (t && t->state == TASK_STATE_SUSPENDED) {
            code = t->exit_code;
            done = 1;
            break;
        }
        schedule();
    }

    if (done && code == 0)
        serial_print("[RING3] ALL CHECKS PASSED (hello exit 0)\n");
    else if (done) {
        extern void kprintf(const char *fmt, ...);
        kprintf("[RING3] FAIL: hello exit=%d\n", (uint64_t)code);
    } else
        serial_print("[RING3] FAIL: timeout (task nao encerrou)\n");
}

/* ================================================================== */
/* bsdsocket.library — AmigaOS-style BSD socket API                     */
/* ================================================================== */

#include <net/net.h>

int64_t aos_socket(int domain, int type, int protocol, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)Socket(domain, type, protocol);
}

/* Cap for a single socket payload bounce (64 KiB, well above MTU).
 * Larger requests fail with TOO_BIG instead of risking huge kmallocs. */
#define NET_BUF_MAX (64 * 1024)

static int copy_sockaddr_in(struct sockaddr_in *kdst, const struct sockaddr *usrc,
                            int tolen) {
    if (!usrc || tolen < (int)sizeof(struct sockaddr_in)) return -1;
    return copy_from_user(kdst, usrc, sizeof(*kdst));
}

int64_t aos_bind(int sockfd, const struct sockaddr *addr, int addrlen, struct interrupt_frame *frame) {
    (void)frame;
    /* Bounce: never dereference the user sockaddr directly (uaccess P0).
     * Copy to kstack first, then hand a kernel pointer to the driver.
     * Inner Bind() stays kernel-pointer-only for CPL0 callers. */
    struct sockaddr_in kaddr;
    if (copy_sockaddr_in(&kaddr, addr, addrlen) != 0)
        return -AOS_ERR_BAD_ADDRESS;
    return (int64_t)Bind(sockfd, (const struct sockaddr *)&kaddr,
                         (int)sizeof kaddr);
}

int64_t aos_send(int sockfd, const void *buf, int len, int flags, const struct sockaddr *dest_addr, int addrlen, struct interrupt_frame *frame) {
    (void)frame;
    char *kbuf;
    int rc;
    if (!buf || len <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (len > NET_BUF_MAX) return -AOS_ERR_TOO_BIG;
    kbuf = (char *)kmalloc((size_t)len);
    if (!kbuf) return -AOS_ERR_NO_MEMORY;
    if (copy_from_user(kbuf, buf, (size_t)len) != 0) {
        kfree(kbuf);
        return -AOS_ERR_BAD_ADDRESS;
    }
    /* Send() requires connected socket; SendTo() uses dest_addr.
     * We route to SendTo when dest_addr is non-NULL for backward compat. */
    if (dest_addr) {
        struct sockaddr_in kdest;
        if (copy_sockaddr_in(&kdest, dest_addr, addrlen) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
        rc = SendTo(sockfd, kbuf, len, flags,
                    (const struct sockaddr *)&kdest, (int)sizeof kdest);
    } else {
        rc = Send(sockfd, kbuf, len, flags);
    }
    kfree(kbuf);
    return (int64_t)rc;
}

int64_t aos_recv(int sockfd, void *buf, int len, int flags, struct sockaddr *src_addr, int *addrlen, struct interrupt_frame *frame) {
    (void)frame;
    char *kbuf;
    int rc;
    if (!buf || len <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (len > NET_BUF_MAX) return -AOS_ERR_TOO_BIG;
    kbuf = (char *)kmalloc((size_t)len);
    if (!kbuf) return -AOS_ERR_NO_MEMORY;
    if (src_addr && addrlen) {
        /* From-address requested: bounce the sockaddr and the length word.
         * The driver only writes them when rc > 0 — mirror that. */
        struct sockaddr_in ksrc;
        int klen;
        if (copy_from_user(&klen, addrlen, sizeof klen) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
        rc = RecvFrom(sockfd, kbuf, len, flags,
                      (struct sockaddr *)&ksrc, &klen);
        if (rc > 0) {
            if (copy_to_user(buf, kbuf, (size_t)rc) != 0 ||
                copy_to_user(src_addr, &ksrc, sizeof ksrc) != 0 ||
                copy_to_user(addrlen, &klen, sizeof klen) != 0) {
                kfree(kbuf);
                return -AOS_ERR_BAD_ADDRESS;
            }
        }
    } else {
        rc = Recv(sockfd, kbuf, len, flags);
        if (rc > 0 && copy_to_user(buf, kbuf, (size_t)rc) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
    }
    kfree(kbuf);
    return (int64_t)rc;
}

int64_t aos_close_socket(int64_t fd, struct interrupt_frame *frame) {
    (void)frame;
    return (int64_t)CloseSocket((int)fd);
}

int64_t aos_select(int64_t width, uint64_t readfds_ptr, uint64_t writefds_ptr,
                   uint64_t exceptfds_ptr, uint64_t timeout_ptr, struct interrupt_frame *frame) {
    (void)frame;
    (void)writefds_ptr;
    (void)exceptfds_ptr;
    (void)timeout_ptr;
    /* Bounce: inner Select() only implements readfds polling; wfds/efds/
     * timeout are currently ignored, so never touch those user pointers.
     * Copy readfds IN, call with kernel pointer, copy result OUT. */
    bool has_rfds = (readfds_ptr != 0);
    fd_set k_rfds;
    if (has_rfds) {
        if (copy_from_user(&k_rfds, (const void *)readfds_ptr,
                           sizeof k_rfds) != 0)
            return -AOS_ERR_BAD_ADDRESS;
    }
    int rc = Select((int)width, has_rfds ? &k_rfds : NULL,
                    NULL, NULL, NULL);
    if (rc >= 0 && has_rfds) {
        if (copy_to_user((void *)readfds_ptr, &k_rfds, sizeof k_rfds) != 0)
            return -AOS_ERR_BAD_ADDRESS;
    }
    return (int64_t)rc;
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
    /* Bounce: optlen is IN/OUT int, optval is OUT (max 4B for current int
     * options). Driver only writes when *optlen >= 4 — mirror that so a
     * small buffer never leaks kernel stack to userspace. */
    int klen;
    int kval = 0;
    if (!optval_ptr || !optlen_ptr)
        return -AOS_ERR_BAD_ADDRESS;
    if (copy_from_user(&klen, (const void *)optlen_ptr, sizeof klen) != 0)
        return -AOS_ERR_BAD_ADDRESS;
    int rc = GetSockOpt((int)sockfd, (int)level, (int)optname, &kval, &klen);
    if (rc == 0) {
        /* Inner sets klen=4 only when it actually wrote kval; propagate
         * klen always, kval only when written. */
        if (klen == (int)sizeof kval) {
            if (copy_to_user((void *)optval_ptr, &kval, sizeof kval) != 0)
                return -AOS_ERR_BAD_ADDRESS;
        }
        if (copy_to_user((void *)optlen_ptr, &klen, sizeof klen) != 0)
            return -AOS_ERR_BAD_ADDRESS;
    }
    return (int64_t)rc;
}

int64_t aos_get_socket_addr(int64_t sockfd, uint64_t name_ptr, uint64_t namelen_ptr,
                            struct interrupt_frame *frame) {
    (void)frame;
    /* Bounce: namelen IN/OUT + sockaddr OUT. Only copy OUT on success,
     * mirroring the driver (which only writes when rc==0). */
    struct sockaddr_in kname;
    int klen;
    if (!name_ptr || !namelen_ptr)
        return -AOS_ERR_BAD_ADDRESS;
    if (copy_from_user(&klen, (const void *)namelen_ptr, sizeof klen) != 0)
        return -AOS_ERR_BAD_ADDRESS;
    int rc = GetSocketAddr((int)sockfd, (struct sockaddr *)&kname, &klen);
    if (rc == 0) {
        if (copy_to_user((void *)name_ptr, &kname, sizeof kname) != 0)
            return -AOS_ERR_BAD_ADDRESS;
        if (copy_to_user((void *)namelen_ptr, &klen, sizeof klen) != 0)
            return -AOS_ERR_BAD_ADDRESS;
    }
    return (int64_t)rc;
}

int64_t aos_socketioctl(int64_t sockfd, int64_t request, uint64_t arg_ptr,
                        struct interrupt_frame *frame) {
    (void)frame;
    /* Bounce: FIONREAD is OUT int; FIONBIO ignores arg (no touch).
     * Anything else falls through to the driver for BAD_ARGUMENT. */
    if ((int)request == FIONREAD) {
        int knbytes = 0;
        if (!arg_ptr)
            return -AOS_ERR_BAD_ADDRESS;
        int rc = SocketIOCtl((int)sockfd, (int)request, &knbytes);
        if (rc == 0) {
            if (copy_to_user((void *)arg_ptr, &knbytes, sizeof knbytes) != 0)
                return -AOS_ERR_BAD_ADDRESS;
        }
        return (int64_t)rc;
    }
    if ((int)request == FIONBIO) {
        /* Accept without touching user memory (no non-blocking support yet,
         * matches driver stub behavior). */
        return (int64_t)SocketIOCtl((int)sockfd, (int)request, NULL);
    }
    return (int64_t)SocketIOCtl((int)sockfd, (int)request, NULL);
}

int64_t aos_socket_base_tags(uint64_t taglist_ptr, struct interrupt_frame *frame) {
    /* Bounce: TagItem array is user memory terminated by TAG_DONE.
     * Copy one item at a time with a hard cap so a missing TAG_DONE
     * can't OOB/travar o kernel (old code walked unbounded).
     * SBT_Task hijacks sb_Task — only CPL0 may use it; Ring3 gets
     * NO_PERMISSION. Inner SocketBaseTags() stays kernel-pointer-only. */
#define SOCKBT_TAGS_MAX 32
    struct TagItem ktags[SOCKBT_TAGS_MAX + 1];
    bool from_user = caller_is_user(frame);

    if (!taglist_ptr)
        return (int64_t)SocketBaseTags(NULL);

    for (int i = 0; i < SOCKBT_TAGS_MAX; i++) {
        const struct TagItem *uitem =
            (const struct TagItem *)(taglist_ptr + (uint64_t)i * sizeof(struct TagItem));
        if (copy_from_user(&ktags[i], uitem, sizeof ktags[i]) != 0)
            return -AOS_ERR_BAD_ADDRESS;
        if (ktags[i].ti_Tag == TAG_DONE) {
            if (from_user) {
                for (int j = 0; j <= i; j++) {
                    if (ktags[j].ti_Tag == SBT_Task)
                        return -AOS_ERR_NO_PERMISSION;
                }
            }
            return (int64_t)SocketBaseTags(ktags);
        }
        /* TAG_IGNORE and unknown tags are preserved; inner ignores them.
         * Only SBT_Task is security-sensitive (validated above). */
    }
    /* No TAG_DONE within cap — refuse instead of walking into unmapped mem. */
    return -AOS_ERR_TOO_BIG;
#undef SOCKBT_TAGS_MAX
}

int64_t aos_sendto(int64_t sockfd, uint64_t buf_ptr, int64_t len, int64_t flags,
                   uint64_t to_ptr, int64_t tolen, struct interrupt_frame *frame) {
    (void)frame;
    const void *buf = (const void *)buf_ptr;
    const struct sockaddr *to = (const struct sockaddr *)to_ptr;
    char *kbuf;
    int rc;
    if (!buf || len <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (len > NET_BUF_MAX) return -AOS_ERR_TOO_BIG;
    kbuf = (char *)kmalloc((size_t)len);
    if (!kbuf) return -AOS_ERR_NO_MEMORY;
    if (copy_from_user(kbuf, buf, (size_t)len) != 0) {
        kfree(kbuf);
        return -AOS_ERR_BAD_ADDRESS;
    }
    if (to) {
        struct sockaddr_in kdest;
        if (copy_sockaddr_in(&kdest, to, (int)tolen) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
        rc = SendTo((int)sockfd, kbuf, (int)len, (int)flags,
                    (const struct sockaddr *)&kdest, (int)sizeof kdest);
    } else {
        rc = SendTo((int)sockfd, kbuf, (int)len, (int)flags, NULL, 0);
    }
    kfree(kbuf);
    return (int64_t)rc;
}

int64_t aos_recvfrom(int64_t sockfd, uint64_t buf_ptr, int64_t len, int64_t flags,
                     uint64_t from_ptr, uint64_t fromlen_ptr, struct interrupt_frame *frame) {
    (void)frame;
    void *buf = (void *)buf_ptr;
    char *kbuf;
    int rc;
    if (!buf || len <= 0) return -AOS_ERR_BAD_ARGUMENT;
    if (len > NET_BUF_MAX) return -AOS_ERR_TOO_BIG;
    kbuf = (char *)kmalloc((size_t)len);
    if (!kbuf) return -AOS_ERR_NO_MEMORY;
    if (from_ptr && fromlen_ptr) {
        struct sockaddr_in kfrom;
        int klen;
        if (copy_from_user(&klen, (const void *)fromlen_ptr, sizeof klen) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
        rc = RecvFrom((int)sockfd, kbuf, (int)len, (int)flags,
                      (struct sockaddr *)&kfrom, &klen);
        if (rc > 0) {
            if (copy_to_user((void *)from_ptr, &kfrom, sizeof kfrom) != 0 ||
                copy_to_user((void *)fromlen_ptr, &klen, sizeof klen) != 0 ||
                copy_to_user(buf, kbuf, (size_t)rc) != 0) {
                kfree(kbuf);
                return -AOS_ERR_BAD_ADDRESS;
            }
        }
    } else {
        /* No address requested: the driver never touches from/fromlen. */
        rc = RecvFrom((int)sockfd, kbuf, (int)len, (int)flags, NULL, NULL);
        if (rc > 0 && copy_to_user(buf, kbuf, (size_t)rc) != 0) {
            kfree(kbuf);
            return -AOS_ERR_BAD_ADDRESS;
        }
    }
    kfree(kbuf);
    return (int64_t)rc;
}

/* ================================================================== */
/* AmigaOS assigns + message ports                                     */
/* ================================================================== */

#include <assign.h>
#include <msgport.h>

int64_t aos_assign(const char *name, const char *path, int64_t op, struct interrupt_frame *frame) {
    (void)frame;
    char kname[ASSIGN_MAX_NAME];
    if (!name) return -AOS_ERR_BAD_ARGUMENT;
    /* Bounce user strings: assign_*() dereferences with strcpy/strcmp and
     * is also called from kernel paths (dogin, dos) with kernel strings,
     * so the copy lives here at the trust boundary, not inside assign.c. */
    if (strncpy_from_user(kname, name, sizeof(kname), NULL) != 0)
        return -AOS_ERR_BAD_ARGUMENT;
    if (op == ASSIGN_SET) {
        char kpath[ASSIGN_MAX_PATH];
        if (!path) return -AOS_ERR_BAD_ARGUMENT;
        if (strncpy_from_user(kpath, path, sizeof(kpath), NULL) != 0)
            return -AOS_ERR_BAD_ARGUMENT;
        return (int64_t)assign_set(kname, kpath);
    }
    if (op == ASSIGN_UNSET)
        return (int64_t)assign_unset(kname);
    if (op == ASSIGN_GET) {
        char buf[ASSIGN_MAX_PATH];
        size_t len = 0;
        if (!path) return -AOS_ERR_BAD_ARGUMENT;
        if (assign_lookup(kname, buf, sizeof buf) != 0)
            return -AOS_ERR_NOT_FOUND;
        while (len < sizeof buf && buf[len]) len++;
        if (len >= sizeof buf) return -AOS_ERR_BAD_ARGUMENT;
        if (copy_to_user((void *)path, buf, len + 1) != 0)
            return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
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
        return -AOS_ERR_BAD_ADDRESS;
    return msgport_reply(token, &kernel_msg);
}
