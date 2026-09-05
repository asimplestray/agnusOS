#ifndef EXEC_SIGNAL_H
#define EXEC_SIGNAL_H

#include <exec/task.h>

/*
 * AmigaOS-style signal mechanism.
 *
 * Signals are a 32-bit bitmask sent between tasks.  Unlike POSIX signals
 * there are no handlers, no signal frames, no ucontext.  The sender
 * sets bits in the receiver's et_SigRecv; the receiver calls exec_wait()
 * which blocks until any of the requested bits are set, then returns
 * which bits arrived.
 */

/* Send signal bitmask to a task. */
void exec_signal(exec_task_t *task, uint32_t mask);

/* Wait until ANY bit in mask is set. Returns the bits that arrived. */
uint32_t exec_wait(uint32_t mask);

/* Wait with timeout (ticks). Returns 0 on timeout, or bits on signal. */
uint32_t exec_wait_timeout(uint32_t mask, uint32_t timeout_ticks);

/* Set signal bits on the current task. Returns previous bits. */
uint32_t exec_set_signal(uint32_t mask);

/* Clear signal bits on the current task. Returns previous bits. */
uint32_t exec_clear_signal(uint32_t mask);

/* Check signal bits without blocking (non-blocking). */
uint32_t exec_check_signal(exec_task_t *task);

/* Cause an exception signal on a task (sets bit + calls handler if set). */
void exec_cause(exec_task_t *task, uint32_t mask);

#endif
