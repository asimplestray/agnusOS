#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <task.h>
#include <idt.h>
#include <msgport.h>

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* Errno constants */
#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define ENOTBLK         15
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define ENOSYS          38

/* Open flags */
#define O_RDONLY         0x0000
#define O_WRONLY         0x0001
#define O_RDWR           0x0002
#define O_ACCMODE        0x0003
#define O_CREAT          0x0040
#define O_EXCL           0x0080
#define O_NOCTTY         0x0100
#define O_TRUNC          0x0200
#define O_APPEND         0x0400
#define O_NONBLOCK       0x0800
#define O_SYNC           0x1000
#define O_DIRECTORY      0x2000
#define O_NOFOLLOW       0x4000
#define O_CLOEXEC        0x8000

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_mode;
    uint64_t st_nlink;
    uint64_t st_uid;
    uint64_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_mtime;
    int64_t  st_ctime;
};

/* dirent for getdents */
struct dirent {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

/* Socket structures */
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

/* ApolloOS DOS-trap dispatch table.
 * Naming follows AmigaDOS / Exec.library conventions while keeping the
 * numeric ABI identical to the previous Linux-like layout. */

#define AOS_Exit            0   /* was SYS_EXIT        */
#define AOS_SpawnTask       1   /* was SYS_FORK        */
#define AOS_Read            2   /* was SYS_READ        */
#define AOS_Write           3   /* was SYS_WRITE       */
#define AOS_Open            4   /* was SYS_OPEN        */
#define AOS_Close           5   /* was SYS_CLOSE       */
#define AOS_Wait            6   /* was SYS_WAITPID     */
#define AOS_LoadSeg         7   /* was SYS_EXECVE      */
#define AOS_SetBrk          8   /* was SYS_BRK         */
#define AOS_AllocMem        9   /* was SYS_MMAP        */
#define AOS_FreeMem        10   /* was SYS_MUNMAP      */
#define AOS_DoIO           11   /* was SYS_IOCTL       */
#define AOS_FindTask       12   /* was SYS_GETPID      */
#define AOS_Yield          13   /* was SYS_YIELD       */
#define AOS_Delay          14   /* was SYS_NANOSLEEP   */
#define AOS_GetSysTime     15   /* was SYS_GETTIME     */
#define AOS_AddTask        16   /* was SYS_SPAWN       */
#define AOS_Signal         17   /* was SYS_RT_SIGACTION  */
#define AOS_SetSignal      18   /* was SYS_RT_SIGPROCMASK */
#define AOS_ReturnSignal   19   /* was SYS_RT_SIGRETURN   */
#define AOS_SendSignal     20   /* was SYS_KILL         */
#define AOS_Pipe           21   /* was SYS_PIPE         */
#define AOS_Seek           22   /* was SYS_LSEEK        */
#define AOS_Examine        23   /* was SYS_STAT         */
#define AOS_Clock          24   /* was SYS_CLOCK_GETTIME*/
#define AOS_PutStr         25   /* was SYS_SYSLOG       */
#define AOS_IoErr          26   /* was SYS_GET_ERRNO    */
#define AOS_SetIoErr       27   /* was SYS_SET_ERRNO    */
#define AOS_SetProcGroup   28   /* was SYS_SETPGID      */
#define AOS_GetProcGroup   29   /* was SYS_GETPGID      */
#define AOS_SetConProc     30   /* was SYS_TCSETPGRP    */
#define AOS_GetConProc     31   /* was SYS_TCGETPGRP    */
#define AOS_CreateDir      32   /* was SYS_MKDIR        */
#define AOS_DeleteDir      33   /* was SYS_RMDIR        */
#define AOS_DeleteFile     34   /* was SYS_UNLINK       */
#define AOS_CurrentDir     35   /* was SYS_CHDIR        */
#define AOS_CurrentDirFD   36   /* was SYS_FCHDIR       */
#define AOS_LockCWD        37   /* was SYS_GETCWD       */
#define AOS_Rename         38   /* was SYS_RENAME       */
#define AOS_ExamineDir     39   /* was SYS_GETDENTS     */
#define AOS_Socket         40   /* was SYS_SOCKET       */
#define AOS_Bind           41   /* was SYS_BIND         */
#define AOS_Send           42   /* was SYS_SENDTO       */
#define AOS_Recv           43   /* was SYS_RECVFROM     */
#define AOS_CloseSocket    44   /* was SYS_SOCK_CLOSE   */
#define AOS_Flush          45   /* was SYS_FSYNC        */
#define AOS_Assign         46   /* Amiga assign         */
#define AOS_CreatePort     47   /* was SYS_MSGPORT_CREATE */
#define AOS_DeletePort     48   /* was SYS_MSGPORT_DELETE */
#define AOS_PutMsg         49   /* was SYS_MSGPORT_PUT    */
#define AOS_GetMsg         50   /* was SYS_MSGPORT_GET    */
#define AOS_WaitPort       51   /* was SYS_MSGPORT_WAIT   */
#define AOS_ReplyMsg       52   /* was SYS_MSGPORT_REPLY  */

#define NR_SYSCALLS         53

/* Compat aliases (keep old SYS_ working during transition) */
#define SYS_EXIT SYS_EXIT
#define SYS_FORK AOS_SpawnTask
#define SYS_READ AOS_Read
#define SYS_WRITE AOS_Write
#define SYS_OPEN AOS_Open
#define SYS_CLOSE AOS_Close
#define SYS_WAITPID AOS_Wait
#define SYS_EXECVE AOS_LoadSeg
#define SYS_BRK AOS_SetBrk
#define SYS_MMAP AOS_AllocMem
#define SYS_MUNMAP AOS_FreeMem
#define SYS_IOCTL AOS_DoIO
#define SYS_GETPID AOS_FindTask
#define SYS_YIELD AOS_Yield
#define SYS_NANOSLEEP AOS_Delay
#define SYS_GETTIME AOS_GetSysTime
#define SYS_SPAWN AOS_AddTask

int64_t aos_exit(int64_t code, struct interrupt_frame *frame);
int64_t aos_spawn_task(struct interrupt_frame *frame);
int64_t aos_read(int64_t fd, void *buf, int64_t count, struct interrupt_frame *frame);
int64_t aos_write(int64_t fd, const void *buf, int64_t count, struct interrupt_frame *frame);
int64_t aos_open(const char *pathname, int64_t flags, int64_t mode, struct interrupt_frame *frame);
int64_t aos_close(int64_t fd, struct interrupt_frame *frame);
int64_t aos_wait(int64_t pid, int64_t *status, int64_t options, struct interrupt_frame *frame);
int64_t aos_loadseg(const char *path, const char **argv, const char **envp, struct interrupt_frame *frame);
int64_t aos_setbrk(void *addr, struct interrupt_frame *frame);
void *aos_allocmem(void *addr, int64_t length, int64_t prot, int64_t flags, int64_t fd, int64_t offset, struct interrupt_frame *frame);
int64_t aos_freemem(void *addr, int64_t length, struct interrupt_frame *frame);
int64_t aos_doio(int64_t fd, uint64_t request, void *arg, struct interrupt_frame *frame);
int64_t aos_find_task(struct interrupt_frame *frame);
int64_t aos_yield(struct interrupt_frame *frame);
int64_t aos_delay(const struct timespec *req, struct timespec *rem, struct interrupt_frame *frame);
int64_t aos_getsystime(struct timespec *ts, struct interrupt_frame *frame);
int64_t aos_clock(int64_t clk_id, struct timespec *ts, struct interrupt_frame *frame);
int64_t aos_putstr(int type, char *buf, int len, struct interrupt_frame *frame);
int64_t aos_socket(int domain, int type, int protocol, struct interrupt_frame *frame);
int64_t aos_bind(int sockfd, const struct sockaddr *addr, int addrlen, struct interrupt_frame *frame);
int64_t aos_send(int sockfd, const void *buf, int len, int flags, const struct sockaddr *dest_addr, int addrlen, struct interrupt_frame *frame);
int64_t aos_recv(int sockfd, void *buf, int len, int flags, struct sockaddr *src_addr, int *addrlen, struct interrupt_frame *frame);
int64_t aos_close_socket(int64_t fd, struct interrupt_frame *frame);
int64_t aos_addtask(void (*entry)(void), uint64_t flags, struct interrupt_frame *frame);
int64_t aos_signal(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize, struct interrupt_frame *frame);
int64_t aos_setsignal(int how, const sigset_t *set, sigset_t *oldset, size_t sigsetsize, struct interrupt_frame *frame);
int64_t aos_return_signal(struct interrupt_frame *frame);
int64_t aos_send_signal(int64_t pid, int64_t sig, struct interrupt_frame *frame);
int64_t aos_pipe(int64_t pipefd[2], struct interrupt_frame *frame);
int64_t aos_seek(int64_t fd, int64_t offset, int64_t whence, struct interrupt_frame *frame);
int64_t aos_examine(const char *pathname, struct stat *statbuf, struct interrupt_frame *frame);
int64_t aos_ioerr(struct interrupt_frame *frame);
int64_t aos_set_ioerr(int64_t errno_val, struct interrupt_frame *frame);
int64_t aos_set_procgroup(int64_t pid, int64_t pgid, struct interrupt_frame *frame);
int64_t aos_get_procgroup(int64_t pid, struct interrupt_frame *frame);
int64_t aos_set_conproc(int64_t fd, int64_t pgid, struct interrupt_frame *frame);
int64_t aos_get_conproc(int64_t fd, struct interrupt_frame *frame);
int64_t aos_create_dir(const char *pathname, int64_t mode, struct interrupt_frame *frame);
int64_t aos_delete_dir(const char *pathname, struct interrupt_frame *frame);
int64_t aos_delete_file(const char *pathname, struct interrupt_frame *frame);
int64_t aos_current_dir(const char *pathname, struct interrupt_frame *frame);
int64_t aos_current_dir_fd(int64_t fd, struct interrupt_frame *frame);
int64_t aos_lock_cwd(char *buf, int64_t size, struct interrupt_frame *frame);
int64_t aos_rename(const char *oldpath, const char *newpath, struct interrupt_frame *frame);
int64_t aos_examine_dir(int64_t fd, struct dirent *dirp, int64_t count, struct interrupt_frame *frame);
int64_t aos_flush(int64_t fd, struct interrupt_frame *frame);

/* AmigaOS-style assigns + message ports */
int64_t aos_assign(const char *name, const char *path, int64_t op, struct interrupt_frame *frame);
int64_t aos_create_port(const char *name, struct interrupt_frame *frame);
int64_t aos_delete_port(int64_t id, struct interrupt_frame *frame);
int64_t aos_put_msg(int64_t id, const msg_t *msg, struct interrupt_frame *frame);
int64_t aos_get_msg(int64_t id, msg_t *msg, int64_t *token, struct interrupt_frame *frame);
int64_t aos_wait_port(int64_t id, int64_t timeout_ms, struct interrupt_frame *frame);
int64_t aos_reply_msg(int64_t token, const msg_t *msg, struct interrupt_frame *frame);

void syscall_init(void);
void syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, struct interrupt_frame *frame);

/* Launch a new user-mode process from an ELF file in RamFS */
int64_t task_create_user(const char *path);

#endif