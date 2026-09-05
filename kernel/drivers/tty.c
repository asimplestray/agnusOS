#include <tty.h>
#include <task.h>
#include <kheap.h>
#include <screen.h>
#include <spinlock.h>
#include <syscall.h>

tty_struct_t *console_tty = NULL;

static void tty_init_termios(termios_t *t) {
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    
    t->c_cc[VINTR]   = 3;   /* Ctrl+C */
    t->c_cc[VQUIT]   = 28;  /* Ctrl+\ */
    t->c_cc[VERASE]  = 127; /* DEL/Backspace */
    t->c_cc[VKILL]   = 21;  /* Ctrl+U */
    t->c_cc[VEOF]    = 4;   /* Ctrl+D */
    t->c_cc[VTIME]   = 0;
    t->c_cc[VMIN]    = 1;
    t->c_cc[VSUSP]   = 26;  /* Ctrl+Z */
    t->c_cc[VSTART]  = 17;  /* Ctrl+Q */
    t->c_cc[VSTOP]   = 19;  /* Ctrl+S */
    t->c_cc[VREPRINT] = 18; /* Ctrl+R */
    t->c_cc[VDISCARD] = 15; /* Ctrl+O */
    t->c_cc[VWERASE] = 23;  /* Ctrl+W */
    t->c_cc[VLNEXT]  = 22;  /* Ctrl+V */
}

void tty_init(void) {
    console_tty = (tty_struct_t *)kmalloc(sizeof(tty_struct_t));
    if (!console_tty) return;
    
    console_tty->canon_head = 0;
    console_tty->canon_tail = 0;
    console_tty->canon_len = 0;
    console_tty->raw_head = 0;
    console_tty->raw_tail = 0;
    console_tty->out_head = 0;
    console_tty->out_tail = 0;
    console_tty->fg_pgrp = 1;  /* init process group */
    console_tty->next = NULL;
    
    init_waitqueue_head(&console_tty->read_wait);
    init_waitqueue_head(&console_tty->write_wait);
    
    tty_init_termios(&console_tty->termios);
}

int tty_putc(tty_struct_t *tty, char c) {
    if (!tty) return -1;
    
    unsigned long flags;
    spinlock_irq_t lock;
    spin_lock_irqsave(&lock, &flags);
    
    int next = (tty->out_head + 1) % TTY_BUF_SIZE;
    if (next == tty->out_tail) {
        spin_unlock_irqrestore(&lock, flags);
        return -1; /* Buffer full */
    }
    
    tty->out_buf[tty->out_head] = c;
    tty->out_head = next;
    
    spin_unlock_irqrestore(&lock, flags);
    return 1;
}

int tty_write(tty_struct_t *tty, const char *buf, int len) {
    if (!tty || !buf) return -1;
    
    int written = 0;
    for (int i = 0; i < len; i++) {
        if (tty_putc(tty, buf[i]) < 0) break;
        written++;
    }
    return written;
}

static int tty_canon_buf_full(tty_struct_t *tty) {
    return tty->canon_len >= TTY_BUF_SIZE - 1;
}

static void tty_canon_add(tty_struct_t *tty, char c) {
    if (tty_canon_buf_full(tty)) return;
    
    tty->canon_buf[tty->canon_head] = c;
    tty->canon_head = (tty->canon_head + 1) % TTY_BUF_SIZE;
    tty->canon_len++;
    
    if (c == '\n') {
        wake_up(&tty->read_wait);
    }
}

static void tty_raw_add(tty_struct_t *tty, char c) {
    int next = (tty->raw_head + 1) % TTY_BUF_SIZE;
    if (next == tty->raw_tail) return; /* Buffer full */
    
    tty->raw_buf[tty->raw_head] = c;
    tty->raw_head = next;
    wake_up(&tty->read_wait);
}

static char tty_canon_get(tty_struct_t *tty) {
    if (tty->canon_len == 0) return 0;
    
    char c = tty->canon_buf[tty->canon_tail];
    tty->canon_tail = (tty->canon_tail + 1) % TTY_BUF_SIZE;
    tty->canon_len--;
    return c;
}

static char tty_raw_get(tty_struct_t *tty) {
    if (tty->raw_head == tty->raw_tail) return 0;
    
    char c = tty->raw_buf[tty->raw_tail];
    tty->raw_tail = (tty->raw_tail + 1) % TTY_BUF_SIZE;
    return c;
}

void tty_input_char(tty_struct_t *tty, char c) {
    if (!tty) return;
    
    termios_t *t = &tty->termios;
    
    /* Handle special characters in ISIG mode */
    if (t->c_lflag & ISIG) {
        if (c == t->c_cc[VINTR]) {  /* Ctrl+C */
            tty_handle_ctrl_c(tty);
            return;
        }
        if (c == t->c_cc[VSUSP]) {  /* Ctrl+Z */
            tty_handle_ctrl_z(tty);
            return;
        }
        if (c == t->c_cc[VQUIT]) {  /* Ctrl+\ */
            /* Send SIGQUIT to foreground process group */
            return;
        }
    }
    
    if (t->c_lflag & ICANON) {
        /* Canonical mode - line buffered */
        
        /* Handle EOF (Ctrl+D) */
        if (c == t->c_cc[VEOF]) {
            tty_handle_ctrl_d(tty);
            return;
        }
        
        /* Handle backspace */
        if (c == t->c_cc[VERASE] || c == '\b' || c == 127) {
            tty_handle_backspace(tty);
            return;
        }
        
        /* Handle line kill (Ctrl+U) */
        if (c == t->c_cc[VKILL]) {
            tty->canon_len = 0;
            tty->canon_head = tty->canon_tail;
            if (t->c_lflag & ECHO) {
                screen_print("\r\n");
            }
            return;
        }
        
        /* Handle reprint (Ctrl+R) */
        if (c == t->c_cc[VREPRINT]) {
            if (t->c_lflag & ECHO) {
                screen_print("\r\n");
                for (int i = 0; i < tty->canon_len; i++) {
                    int idx = (tty->canon_tail + i) % TTY_BUF_SIZE;
                    screen_putc(tty->canon_buf[idx]);
                }
            }
            return;
        }
        
        /* Handle word erase (Ctrl+W) */
        if (c == t->c_cc[VWERASE]) {
            /* Erase last word */
            int len = tty->canon_len;
            while (len > 0) {
                int idx = (tty->canon_tail + len - 1) % TTY_BUF_SIZE;
                if (tty->canon_buf[idx] == ' ' || tty->canon_buf[idx] == '\t') break;
                len--;
            }
            tty->canon_len = len;
            tty->canon_head = (tty->canon_tail + len) % TTY_BUF_SIZE;
            if (t->c_lflag & ECHO) {
                screen_print("\b \b"); /* Simple backspace visual */
            }
            return;
        }
        
        /* Handle literal next (Ctrl+V) */
        if (c == t->c_cc[VLNEXT]) {
            if (t->c_lflag & ECHO) {
                screen_print("^V");
            }
            return;
        }
        
        /* Handle newline */
        if (c == '\n' || c == '\r') {
            tty_canon_add(tty, '\n');
            if (t->c_lflag & ECHO) {
                screen_putc('\n');
            }
            return;
        }
        
        /* Regular character */
        if (c >= 32 && c < 127) {
            tty_canon_add(tty, c);
            if (t->c_lflag & ECHO) {
                screen_putc(c);
            }
        }
    } else {
        /* Raw mode */
        tty_raw_add(tty, c);
        if (t->c_lflag & ECHO) {
            screen_putc(c);
        }
    }
}

void tty_handle_ctrl_c(tty_struct_t *tty) {
    if (!tty) return;
    
    if (tty->termios.c_lflag & ECHO) {
        screen_print("^C\r\n");
    }
    
    /* Send SIGINT (Ctrl+C) to foreground process group */
    task_struct_t *t = task_list;
    do {
        if (t->pgid == (uint64_t)tty->fg_pgrp && t->state != TASK_STATE_SUSPENDED) {
            force_sig(SIGBIT_BREAK, t);
        }
        t = t->next;
    } while (t != task_list);
}

void tty_handle_ctrl_z(tty_struct_t *tty) {
    if (!tty) return;
    
    if (tty->termios.c_lflag & ECHO) {
        screen_print("^Z\r\n");
    }
    
    /* Send SIGTSTP (Ctrl+Z) to foreground process group */
    task_struct_t *t = task_list;
    do {
        if (t->pgid == (uint64_t)tty->fg_pgrp && t->state != TASK_STATE_SUSPENDED) {
            force_sig(SIGBIT_SUSPEND, t);
        }
        t = t->next;
    } while (t != task_list);
}

void tty_handle_ctrl_d(tty_struct_t *tty) {
    if (!tty) return;
    
    if (tty->termios.c_lflag & ICANON) {
        if (tty->canon_len == 0) {
            /* Empty line - return EOF */
            tty_canon_add(tty, 0);  /* Special marker for EOF */
        } else {
            /* Flush current line */
            wake_up(&tty->read_wait);
        }
    }
}

void tty_handle_backspace(tty_struct_t *tty) {
    if (!tty) return;
    
    if (tty->termios.c_lflag & ICANON) {
        if (tty->canon_len > 0) {
            tty->canon_len--;
            tty->canon_head = (tty->canon_head - 1 + TTY_BUF_SIZE) % TTY_BUF_SIZE;
            if (tty->termios.c_lflag & ECHO) {
                if (tty->termios.c_lflag & ECHOE) {
                    screen_print("\b \b");
                } else {
                    screen_putc('\b');
                }
            }
        }
    }
}

int tty_read(tty_struct_t *tty, char *buf, int len) {
    if (!tty || !buf || len <= 0) return -1;

    termios_t *t = &tty->termios;
    int read = 0;

    if (t->c_lflag & ICANON) {
        /* Canonical mode - wait for newline or EOF */
        while (read < len) {
            if (tty->canon_len > 0) {
                char c = tty_canon_get(tty);
                if (c == 0) {  /* EOF marker */
                    break;
                }
                buf[read++] = c;
                if (c == '\n') break;
            } else {
                /* Wait for input */
                wait_event_interruptible(tty->read_wait, tty->canon_len > 0);
                if (current && (current->sig_recv & SIGBIT_BREAK)) {
                    return -EINTR;
                }
            }
        }
    } else {
        /* Raw mode */
        int min = t->c_cc[VMIN];
        int time_val = t->c_cc[VTIME];
        
        while (read < len) {
            char c = tty_raw_get(tty);
            if (c) {
                buf[read++] = c;
                if (min > 0 && read >= min) break;
            } else {
                if (min == 0) {
                    /* Non-blocking: return what we have */
                    break;
                }
                /* Wait for input */
                wait_event_interruptible(tty->read_wait, tty->raw_head != tty->raw_tail);
                if (current && (current->sig_recv & SIGBIT_BREAK)) {
                    return -EINTR;
                }
            }
        }
    }
    
    return read;
}

int tty_ioctl(tty_struct_t *tty, uint64_t request, void *arg) {
    if (!tty) return -1;
    
    switch (request) {
        case TCGETS: {
            termios_t *dst = (termios_t *)arg;
            if (!dst) return -EFAULT;
            *dst = tty->termios;
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            termios_t *src = (termios_t *)arg;
            if (!src) return -EFAULT;
            tty->termios = *src;
            return 0;
        }
        case TCFLSH: {
            int queue = (int)(uintptr_t)arg;
            if (queue == TCIFLUSH || queue == TCIOFLUSH) {
                tty->canon_len = 0;
                tty->canon_head = tty->canon_tail;
                tty->raw_head = tty->raw_tail;
            }
            if (queue == TCOFLUSH || queue == TCIOFLUSH) {
                tty->out_head = tty->out_tail;
            }
            return 0;
        }
        case TIOCGPGRP: {
            int *pgrp = (int *)arg;
            if (!pgrp) return -EFAULT;
            *pgrp = tty->fg_pgrp;
            return 0;
        }
        case TIOCSPGRP: {
            int pgrp = (int)(uintptr_t)arg;
            if (pgrp <= 0) return -EINVAL;
            tty->fg_pgrp = pgrp;
            return 0;
        }
        default:
            return -ENOTTY;
    }
}

int tty_check_fg(tty_struct_t *tty, task_struct_t *task) {
    if (!tty || !task) return 0;
    return task->pgid == (uint64_t)tty->fg_pgrp;
}