#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <task.h>
#include <idt.h>

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

#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_WAITPID     6
#define SYS_EXECVE      7
#define SYS_BRK         8
#define SYS_MMAP        9
#define SYS_MUNMAP      10
#define SYS_IOCTL       11
#define SYS_GETPID      12
#define SYS_YIELD       13
#define SYS_NANOSLEEP   14
#define SYS_GETTIME     15
#define SYS_SPAWN       16
#define SYS_RT_SIGACTION    17
#define SYS_RT_SIGPROCMASK  18
#define SYS_RT_SIGRETURN    19
#define SYS_KILL            20
#define SYS_PIPE            21
#define SYS_LSEEK           22
#define SYS_STAT            23
#define SYS_CLOCK_GETTIME   24
#define SYS_SYSLOG          25
#define SYS_GET_ERRNO       26
#define SYS_SET_ERRNO       27
#define SYS_SETPGID         28
#define SYS_GETPGID         29
#define SYS_TCSETPGRP       30
#define SYS_TCGETPGRP       31
#define SYS_MKDIR           32
#define SYS_RMDIR           33
#define SYS_UNLINK          34
#define SYS_CHDIR           35
#define SYS_FCHDIR          36
#define SYS_GETCWD          37
#define SYS_RENAME          38
#define SYS_GETDENTS        39
#define SYS_SOCKET          40
#define SYS_BIND            41
#define SYS_SENDTO          42
#define SYS_RECVFROM        43
#define SYS_SOCK_CLOSE      44
#define SYS_FSYNC           45

#define NR_SYSCALLS         46

int64_t sys_exit(int64_t code, struct interrupt_frame *frame);
int64_t sys_fork(struct interrupt_frame *frame);
int64_t sys_read(int64_t fd, void *buf, int64_t count, struct interrupt_frame *frame);
int64_t sys_write(int64_t fd, const void *buf, int64_t count, struct interrupt_frame *frame);
int64_t sys_open(const char *pathname, int64_t flags, int64_t mode, struct interrupt_frame *frame);
int64_t sys_close(int64_t fd, struct interrupt_frame *frame);
int64_t sys_waitpid(int64_t pid, int64_t *status, int64_t options, struct interrupt_frame *frame);
int64_t sys_execve(const char *path, const char **argv, const char **envp, struct interrupt_frame *frame);
int64_t sys_brk(void *addr, struct interrupt_frame *frame);
void *sys_mmap(void *addr, int64_t length, int64_t prot, int64_t flags, int64_t fd, int64_t offset, struct interrupt_frame *frame);
int64_t sys_munmap(void *addr, int64_t length, struct interrupt_frame *frame);
int64_t sys_ioctl(int64_t fd, uint64_t request, void *arg, struct interrupt_frame *frame);
int64_t sys_getpid(struct interrupt_frame *frame);
int64_t sys_yield(struct interrupt_frame *frame);
int64_t sys_nanosleep(const struct timespec *req, struct timespec *rem, struct interrupt_frame *frame);
int64_t sys_gettime(struct timespec *ts, struct interrupt_frame *frame);
int64_t sys_clock_gettime(int64_t clk_id, struct timespec *ts, struct interrupt_frame *frame);
int64_t sys_syslog(int type, char *buf, int len, struct interrupt_frame *frame);
int64_t sys_socket(int domain, int type, int protocol, struct interrupt_frame *frame);
int64_t sys_bind(int sockfd, const struct sockaddr *addr, int addrlen, struct interrupt_frame *frame);
int64_t sys_sendto(int sockfd, const void *buf, int len, int flags, const struct sockaddr *dest_addr, int addrlen, struct interrupt_frame *frame);
int64_t sys_recvfrom(int sockfd, void *buf, int len, int flags, struct sockaddr *src_addr, int *addrlen, struct interrupt_frame *frame);
int64_t sys_sock_close(int64_t fd, struct interrupt_frame *frame);
int64_t sys_spawn(void (*entry)(void), uint64_t flags, struct interrupt_frame *frame);
int64_t sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize, struct interrupt_frame *frame);
int64_t sys_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oldset, size_t sigsetsize, struct interrupt_frame *frame);
int64_t sys_rt_sigreturn(struct interrupt_frame *frame);
int64_t sys_kill(int64_t pid, int64_t sig, struct interrupt_frame *frame);
int64_t sys_pipe(int64_t pipefd[2], struct interrupt_frame *frame);
int64_t sys_lseek(int64_t fd, int64_t offset, int64_t whence, struct interrupt_frame *frame);
int64_t sys_stat(const char *pathname, struct stat *statbuf, struct interrupt_frame *frame);
int64_t sys_get_errno(struct interrupt_frame *frame);
int64_t sys_set_errno(int64_t errno_val, struct interrupt_frame *frame);
int64_t sys_setpgid(int64_t pid, int64_t pgid, struct interrupt_frame *frame);
int64_t sys_getpgid(int64_t pid, struct interrupt_frame *frame);
int64_t sys_tcsetpgrp(int64_t fd, int64_t pgid, struct interrupt_frame *frame);
int64_t sys_tcgetpgrp(int64_t fd, struct interrupt_frame *frame);
int64_t sys_mkdir(const char *pathname, int64_t mode, struct interrupt_frame *frame);
int64_t sys_rmdir(const char *pathname, struct interrupt_frame *frame);
int64_t sys_unlink(const char *pathname, struct interrupt_frame *frame);
int64_t sys_chdir(const char *pathname, struct interrupt_frame *frame);
int64_t sys_fchdir(int64_t fd, struct interrupt_frame *frame);
int64_t sys_getcwd(char *buf, int64_t size, struct interrupt_frame *frame);
int64_t sys_rename(const char *oldpath, const char *newpath, struct interrupt_frame *frame);
int64_t sys_getdents(int64_t fd, struct dirent *dirp, int64_t count, struct interrupt_frame *frame);
int64_t sys_fsync(int64_t fd, struct interrupt_frame *frame);

void syscall_init(void);
void syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, struct interrupt_frame *frame);

/* Launch a new user-mode process from an ELF file in RamFS */
int64_t task_create_user(const char *path);

#endif