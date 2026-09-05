;; task_switch.S — Assembly context switch for ApolloOS
;;
;; void context_switch(task_struct_t *prev, task_struct_t *next);
;;   rdi = prev, rsi = next
;;
;; Saves/restores callee-saved registers and kernel RSP.
;; Uses `ret` to resume — callers see a normal return from schedule().
;;
;; NOTE: No `sti` here. Interrupt state is managed by the caller:
;; schedule() calls RUNQUEUE_UNLOCK(flags) before context_switch(),
;; which restores IF to its pre-lock value.

%define TASK_RSP    96     ; offsetof(task_struct_t, rsp)

section .text

global context_switch
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi + TASK_RSP], rsp       ; prev->rsp = rsp
    mov rsp, [rsi + TASK_RSP]       ; rsp = next->rsp

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret
