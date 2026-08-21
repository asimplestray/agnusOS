#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <wait.h>

typedef struct task_struct task_struct_t;

#define TTY_BUF_SIZE 4096

/* Termios flags - input modes */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0001000
#define IXOFF   0002000

/* Termios flags - output modes */
#define OPOST   0000001
#define ONLCR   0000002

/* Termios flags - control modes */
#define CSIZE   0000060
#define   CS5   0000000
#define   CS6   0000020
#define   CS7   0000040
#define   CS8   0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000

/* Termios flags - local modes */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0010000

/* Special characters indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16
#define NCCS     19

/* ioctl requests */
#define TCGETS       0x5401
#define TCSETS       0x5402
#define TCSETSW      0x5403
#define TCSETSF      0x5404
#define TCFLSH       0x540B
#define TIOCGPGRP    0x540F
#define TIOCSPGRP    0x5410

/* TCFLSH queue selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

typedef struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    unsigned char c_cc[NCCS];
} termios_t;

typedef struct tty_struct {
    /* Input buffer (canonical mode) */
    char canon_buf[TTY_BUF_SIZE];
    int canon_head;
    int canon_tail;
    int canon_len;
    
    /* Raw input buffer */
    char raw_buf[TTY_BUF_SIZE];
    int raw_head;
    int raw_tail;
    
    wait_queue_head_t read_wait;
    wait_queue_head_t write_wait;
    
    termios_t termios;
    int fg_pgrp;  /* Foreground process group */
    
    /* Output buffer */
    char out_buf[TTY_BUF_SIZE];
    int out_head;
    int out_tail;
    
    struct tty_struct *next;
} tty_struct_t;

/* Global TTY for console */
extern tty_struct_t *console_tty;

void tty_init(void);
int tty_putc(tty_struct_t *tty, char c);
int tty_write(tty_struct_t *tty, const char *buf, int len);
int tty_read(tty_struct_t *tty, char *buf, int len);
void tty_input_char(tty_struct_t *tty, char c);
void tty_handle_ctrl_c(tty_struct_t *tty);
void tty_handle_ctrl_z(tty_struct_t *tty);
void tty_handle_ctrl_d(tty_struct_t *tty);
void tty_handle_backspace(tty_struct_t *tty);
int tty_ioctl(tty_struct_t *tty, uint64_t request, void *arg);

/* Check if process is in foreground process group */
int tty_check_fg(tty_struct_t *tty, task_struct_t *task);

#endif