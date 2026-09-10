#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <task.h>
#include <idt.h>
#include <msgport.h>

/* ------------------------------------------------------------------ */
/* AgnusOS time — replaces POSIX struct timespec                       */
/* ------------------------------------------------------------------ */

typedef struct aos_timeval {
    int64_t tv_secs;    /* seconds */
    int32_t tv_micros;  /* microseconds */
} aos_timeval_t;

/* ------------------------------------------------------------------ */
/* AmigaOS error codes (replace POSIX errno)                            */
/* ------------------------------------------------------------------ */

#define AOS_ERR_OK              0
#define AOS_ERR_NOT_FOUND       1
#define AOS_ERR_NO_MEMORY       2
#define AOS_ERR_BAD_ARGUMENT    3
#define AOS_ERR_IS_DIRECTORY    4
#define AOS_ERR_NOT_DIRECTORY   5
#define AOS_ERR_FILE_EXISTS     6
#define AOS_ERR_NO_PERMISSION   7
#define AOS_ERR_DISK_FULL       8
#define AOS_ERR_DEVICE_BUSY     9
#define AOS_ERR_TIMEOUT        10
#define AOS_ERR_TOO_BIG        11
#define AOS_ERR_READ_ONLY      12
#define AOS_ERR_NOT_EXECUTABLE 13
#define AOS_ERR_NO_PORT        14
#define AOS_ERR_NO_SIGNAL      15
#define AOS_ERR_MAX            16

/* Legacy errno compat (maps to AOS_ERR_*) */
#define ENOENT    AOS_ERR_NOT_FOUND
#define ENOMEM    AOS_ERR_NO_MEMORY
#define EINVAL    AOS_ERR_BAD_ARGUMENT
#define EISDIR    AOS_ERR_IS_DIRECTORY
#define ENOTDIR   AOS_ERR_NOT_DIRECTORY
#define EEXIST    AOS_ERR_FILE_EXISTS
#define EACCES    AOS_ERR_NO_PERMISSION
#define ENOSPC    AOS_ERR_DISK_FULL
#define EBUSY     AOS_ERR_DEVICE_BUSY
#define ETIMEDOUT AOS_ERR_TIMEOUT
#define E2BIG     AOS_ERR_TOO_BIG
#define EROFS     AOS_ERR_READ_ONLY
#define ENOEXEC   AOS_ERR_NOT_EXECUTABLE
#define EPERM     AOS_ERR_NO_PERMISSION
#define ESRCH     AOS_ERR_NOT_FOUND
#define ECHILD    AOS_ERR_NOT_FOUND
#define EBADF     AOS_ERR_BAD_ARGUMENT
#define EMFILE    AOS_ERR_NO_MEMORY
#define ENOTTY    AOS_ERR_BAD_ARGUMENT
#define ERANGE    AOS_ERR_BAD_ARGUMENT
#define ENOSYS    AOS_ERR_NOT_FOUND
#define EPIPE     AOS_ERR_NOT_FOUND
#define EINTR     AOS_ERR_TIMEOUT
#define EFAULT    AOS_ERR_BAD_ARGUMENT
#define ENODEV    AOS_ERR_NOT_FOUND
#define ENOTCONN  AOS_ERR_NOT_FOUND
#define ETIME     AOS_ERR_TIMEOUT
#define ENOTEMPTY AOS_ERR_IS_DIRECTORY
#define EIO       AOS_ERR_READ_ERROR

/* ------------------------------------------------------------------ */
/* DOS file info — replaces POSIX struct stat                           */
/* ------------------------------------------------------------------ */

/* Note: file_info_block_t is defined in <dos/dos.h> */

/* ------------------------------------------------------------------ */
/* DOS directory entry — replaces POSIX struct dirent                    */
/* ------------------------------------------------------------------ */

typedef struct aos_dir_entry {
    char     aod_Name[108];
    int32_t  aod_Type;    /* >0 = dir, <0 = file */
    int32_t  aod_Size;
} aos_dir_entry_t;

/* ------------------------------------------------------------------ */
/* Open flags — simplified AmigaDOS mode bits                            */
/* ------------------------------------------------------------------ */

#define AOS_O_RDONLY   0   /* MODE_OLDFILE read */
#define AOS_O_WRONLY   1   /* MODE_NEWFILE write */
#define AOS_O_RDWR     2   /* read+write */
#define AOS_O_CREAT    4
#define AOS_O_TRUNC    8
#define AOS_O_APPEND  16

/* ------------------------------------------------------------------ */
/* Socket types                                                         */
/* ------------------------------------------------------------------ */

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char     sin_zero[8];
};

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* ------------------------------------------------------------------ */
/* AgnusOS trap numbers — frozen in uAPI 1.0                           */
/*                                                                     */
/* Naming follows AmigaDOS/Exec conventions. Numeric ABI is stable.     */
/* ------------------------------------------------------------------ */

#define AOS_Exit            0   /* task exit */
#define AOS_SpawnTask       1   /* CreateTask (AmigaOS fork replacement) */
#define AOS_Read            2   /* dos_read via DOS handle */
#define AOS_Write           3   /* dos_write via DOS handle */
#define AOS_Open            4   /* dos_open */
#define AOS_Close           5   /* dos_close */
#define AOS_Wait            6   /* exec_wait (signal bitmask wait) */
#define AOS_LoadSeg         7   /* LoadSeg — load ELF into new task */
#define AOS_SetBrk          8   /* exec_alloc_mem (heap growth) */
#define AOS_AllocMem        9   /* exec_alloc_mem with MEMF flags */
#define AOS_FreeMem        10   /* exec_free_mem */
#define AOS_CreatePool     11   /* exec_create_pool */
#define AOS_DeletePool     12   /* exec_delete_pool */
#define AOS_AllocPooled    13   /* exec_alloc_pooled */
#define AOS_FreePooled     14   /* exec_free_pooled */
#define AOS_PoolAvail      15   /* exec_pool_available */
#define AOS_DoIO           16   /* ioctl — TTY/DRM control */
#define AOS_FindTask       12   /* exec find current task */
#define AOS_Yield          13   /* yield to scheduler */
#define AOS_Delay          14   /* exec_delay (ticks) */
#define AOS_GetSysTime     20   /* exec_eclock */
#define AOS_AddTask        21   /* exec_create_task (kernel only) */
#define AOS_Signal         22   /* exec_signal (set signal bits) */
#define AOS_SetSignal      23   /* exec_set_signal (swap mask) */
#define AOS_ReturnSignal   24   /* exec_check_signal */
#define AOS_SendSignal     25   /* exec_signal on another task */
#define AOS_Pipe           26   /* create MsgPort pair (replaces pipe) */
#define AOS_Seek           27   /* dos_seek */
#define AOS_Examine        28   /* dos_examine → file_info_block_t */
#define AOS_Clock          29   /* exec_eclock */
#define AOS_PutStr         30   /* serial/log output */
#define AOS_IoErr          31   /* dos_io_err */
#define AOS_SetIoErr       32   /* dos_set_io_err */
#define AOS_CreateDir      33   /* dos_create_dir */
#define AOS_DeleteDir      34   /* dos_delete_file (dirs) */
#define AOS_DeleteFile     35   /* dos_delete_file */
#define AOS_CurrentDir     36   /* dos_current_dir */
#define AOS_CurrentDirFD   37   /* dos_current_dir (by BPTR) */
#define AOS_LockCWD        38   /* get current assign path */
#define AOS_Rename         39   /* dos_rename */
#define AOS_ExamineDir     40   /* dos_ex_next → file_info_block_t */
#define AOS_Socket         41   /* network socket */
#define AOS_Bind           42   /* network bind */
#define AOS_Send           43   /* network send */
#define AOS_Recv           44   /* network recv */
#define AOS_CloseSocket    45   /* network close */
#define AOS_Flush          46   /* dos_flush */
#define AOS_Assign         47   /* assign_set/lookup/unset */
#define AOS_CreatePort     48   /* msgport_create */
#define AOS_DeletePort     49   /* msgport_delete */
#define AOS_PutMsg         50   /* msgport_put */
#define AOS_GetMsg         51   /* msgport_get */
#define AOS_WaitPort       52   /* msgport_wait */
#define AOS_ReplyMsg       53   /* msgport_reply */
/* bsdsocket.library */
#define AOS_Select         54
#define AOS_SetSockOpt     55
#define AOS_GetSockOpt     56
#define AOS_GetSocketAddr  57
#define AOS_SocketIOCtl    58
#define AOS_SocketBaseTags 59
#define AOS_SendTo         60
#define AOS_RecvFrom       61

#define NR_SYSCALLS         67

/* ------------------------------------------------------------------ */
/* Syscall prototypes                                                   */
/* ------------------------------------------------------------------ */

int64_t aos_exit(int64_t code, struct interrupt_frame *frame);
int64_t aos_spawn_task(int64_t entry_addr, int64_t stack_size, struct interrupt_frame *frame);
int64_t aos_read(int64_t handle, void *buf, int64_t count, struct interrupt_frame *frame);
int64_t aos_write(int64_t handle, const void *buf, int64_t count, struct interrupt_frame *frame);
int64_t aos_open(const char *name, int64_t mode, struct interrupt_frame *frame);
int64_t aos_close(int64_t handle, struct interrupt_frame *frame);
int64_t aos_wait(int64_t signal_bits, int64_t timeout_ms, struct interrupt_frame *frame);
int64_t aos_loadseg(const char *path, struct interrupt_frame *frame);
int64_t aos_setbrk(int64_t size, struct interrupt_frame *frame);
int64_t aos_allocmem(int64_t size, int64_t mem_flags, struct interrupt_frame *frame);
int64_t aos_freemem(int64_t addr, int64_t size, struct interrupt_frame *frame);
int64_t aos_create_pool(int64_t flags, int64_t pudge_size, int64_t thresh_size, struct interrupt_frame *frame);
int64_t aos_delete_pool(int64_t pool_ptr, struct interrupt_frame *frame);
int64_t aos_alloc_pooled(int64_t pool_ptr, int64_t size, struct interrupt_frame *frame);
int64_t aos_free_pooled(int64_t pool_ptr, int64_t ptr, int64_t size, struct interrupt_frame *frame);
int64_t aos_pool_avail(int64_t pool_ptr, int64_t flags, struct interrupt_frame *frame);
int64_t aos_doio(int64_t handle, uint64_t request, void *arg, struct interrupt_frame *frame);
int64_t aos_find_task(struct interrupt_frame *frame);
int64_t aos_yield(struct interrupt_frame *frame);
int64_t aos_delay(int64_t ticks, struct interrupt_frame *frame);
int64_t aos_getsystime(aos_timeval_t *tv, struct interrupt_frame *frame);
int64_t aos_clock(struct interrupt_frame *frame);
int64_t aos_putstr(int type, char *buf, int len, struct interrupt_frame *frame);
int64_t aos_socket(int domain, int type, int protocol, struct interrupt_frame *frame);
int64_t aos_bind(int sockfd, const struct sockaddr *addr, int addrlen, struct interrupt_frame *frame);
int64_t aos_send(int sockfd, const void *buf, int len, int flags, const struct sockaddr *dest_addr, int addrlen, struct interrupt_frame *frame);
int64_t aos_recv(int sockfd, void *buf, int len, int flags, struct sockaddr *src_addr, int *addrlen, struct interrupt_frame *frame);
int64_t aos_close_socket(int64_t fd, struct interrupt_frame *frame);
int64_t aos_addtask(void (*entry)(void), uint64_t flags, struct interrupt_frame *frame);
int64_t aos_signal(int64_t signal_bits, struct interrupt_frame *frame);
int64_t aos_setsignal(int64_t new_mask, struct interrupt_frame *frame);
int64_t aos_return_signal(struct interrupt_frame *frame);
int64_t aos_send_signal(int64_t pid, int64_t signal_bits, struct interrupt_frame *frame);
int64_t aos_pipe(int64_t port_ids[2], struct interrupt_frame *frame);
int64_t aos_seek(int64_t handle, int64_t position, int64_t offset_type, struct interrupt_frame *frame);
int64_t aos_examine(int64_t lock, void *fib_buf, int64_t fib_size, struct interrupt_frame *frame);
int64_t aos_ioerr(struct interrupt_frame *frame);
int64_t aos_set_ioerr(int64_t err, struct interrupt_frame *frame);
int64_t aos_create_dir(const char *name, struct interrupt_frame *frame);
int64_t aos_delete_dir(const char *name, struct interrupt_frame *frame);
int64_t aos_delete_file(const char *name, struct interrupt_frame *frame);
int64_t aos_current_dir(const char *name, struct interrupt_frame *frame);
int64_t aos_current_dir_fd(int64_t handle, struct interrupt_frame *frame);
int64_t aos_lock_cwd(char *buf, int64_t size, struct interrupt_frame *frame);
int64_t aos_rename(const char *old_name, const char *new_name, struct interrupt_frame *frame);
int64_t aos_examine_dir(int64_t lock, void *fib_buf, int64_t fib_size, struct interrupt_frame *frame);
int64_t aos_flush(int64_t handle, struct interrupt_frame *frame);

/* AmigaOS-style assigns + message ports */
int64_t aos_assign(const char *name, const char *path, int64_t op, struct interrupt_frame *frame);
int64_t aos_create_port(const char *name, struct interrupt_frame *frame);
int64_t aos_delete_port(int64_t id, struct interrupt_frame *frame);
int64_t aos_put_msg(int64_t id, const msg_t *msg, struct interrupt_frame *frame);
int64_t aos_get_msg(int64_t id, msg_t *msg, int64_t *token, struct interrupt_frame *frame);
int64_t aos_wait_port(int64_t id, int64_t timeout_ms, struct interrupt_frame *frame);
int64_t aos_reply_msg(int64_t token, const msg_t *msg, struct interrupt_frame *frame);

/* bsdsocket.library syscalls */
int64_t aos_select(int64_t width, uint64_t readfds_ptr, uint64_t writefds_ptr,
                   uint64_t exceptfds_ptr, uint64_t timeout_ptr, struct interrupt_frame *frame);
int64_t aos_setsockopt(int64_t sockfd, int64_t level, int64_t optname,
                       uint64_t optval_ptr, int64_t optlen, struct interrupt_frame *frame);
int64_t aos_getsockopt(int64_t sockfd, int64_t level, int64_t optname,
                       uint64_t optval_ptr, uint64_t optlen_ptr, struct interrupt_frame *frame);
int64_t aos_get_socket_addr(int64_t sockfd, uint64_t name_ptr, uint64_t namelen_ptr,
                            struct interrupt_frame *frame);
int64_t aos_socketioctl(int64_t sockfd, int64_t request, uint64_t arg_ptr,
                        struct interrupt_frame *frame);
int64_t aos_socket_base_tags(uint64_t taglist_ptr, struct interrupt_frame *frame);
int64_t aos_sendto(int64_t sockfd, uint64_t buf_ptr, int64_t len, int64_t flags,
                   uint64_t to_ptr, int64_t tolen, struct interrupt_frame *frame);
int64_t aos_recvfrom(int64_t sockfd, uint64_t buf_ptr, int64_t len, int64_t flags,
                     uint64_t from_ptr, uint64_t fromlen_ptr, struct interrupt_frame *frame);

void syscall_init(void);
void syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, struct interrupt_frame *frame);

/* Load ELF into a new user task */
int64_t task_create_user(const char *path);

#endif
