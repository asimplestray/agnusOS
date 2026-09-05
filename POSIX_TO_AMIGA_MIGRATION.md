# ApolloOS — Migração: POSIX → AmigaOS-like Moderno

> **Objetivo:** Remover APIs POSIX (signals, fork, fd table, stat, errno, etc.) e substituir por equivalentes AmigaOS-like, mantendo MMU, preempt, GPU, e arquitetura moderna.

---

## Princípio da Migração

```
ANTES:  AOS_SpawnTask() → fork() + COW + argv/envp
DEPOIS: AOS_CreateTask() → CreateTask("nome", pri, entry, stacksize)

ANTES:  AOS_Signal(task, SIGUSR1) → sigaction handler em user stack
DEPOIS: AOS_Signal(task, 0x00002000) → bitmask em tc_SigRecv

ANTES:  AOS_Open("/fat32/test.txt", O_RDONLY) → fd int
DEPOIS: AOS_Open("Work:test.txt", MODE_OLDFILE) → BPTR (handle pointer)

ANTES:  AOS_Wait(pid, WNOHANG) → PID-based wait
DEPOIS: AOS_Wait(mask) → bitmask-based wait (como AmigaOS)
```

---

## Fase 1 — Tipos Fundamentais (sem quebrar nada)

> Criar novos tipos/structs AO LADO dos existentes. Nada é removido ainda.

### 1.1 Novos tipos de memória

```c
// kernel/include/exec/types.h (NOVO)
#pragma once
#include <stdint.h>

typedef uint32_t BPTR;    // "ponteiro" AmigaDOS (offset ou handle)
typedef uint32_t APTR;    // pointer genérico (compat)
typedef uint32_t uLONG;
typedef uint16_t uWORD;
typedef uint8_t  uBYTE;

// Memory flags
#define MEMF_PUBLIC   (1L << 0)   // RAM geral
#define MEMF_CHIP     (1L << 1)   // DMA-accessible (GPU)
#define MEMF_FAST     (1L << 2)   // CPU-only
#define MEMF_CLEAR    (1L << 16)  // zera na alocação
#define MEMF_REVERSE  (1L << 17)  // aloca do fim
#define MEMF_LARGEST  (1L << 18)  // maior bloco disponível

// Error codes (AmigaDOS)
#define AOS_OK             0
#define AOS_ERROR         (-1)
#define AOS_ERR_NO呤      5      // "no such file or directory"
#define AOS_ERR_DIR_NOT_EMPTY 8
#define AOS_ERR_NO_MEMORY 10
#define AOS_ERR_BAD_NUMBER 20
#define AOS_ERR_LINE_TOO_LONG 30
#define AOS_ERR_NO_CONFIG 40
#define AOS_ERR_TOOL刚需 50
```

### 1.2 FileInfoBlock (substitui stat/dirent)

```c
// kernel/include/dos/dos.h (NOVO)
#pragma once
#include <stdint.h>

typedef struct file_info_block {
    int32_t  fib_DiskKey;      // chave interna (inode)
    int32_t  fib_DirEntryType;  // >0 = dir, <0 = file
    char     fib_FileName[108]; // nome do arquivo
    int32_t  fib_Protection;   // permissões (rwxed)
    int32_t  fib_EntryType;    // tipo de entrada
    int32_t  fib_Size;         // tamanho em bytes
    int32_t  fib_NumBlocks;    // blocos alocados
    int8_t   fib_Date[12];     // data/hora (DSTamp)
    char     fib_Comment[80];  // comentário AmigaDOS
    uint16_t fib_OwnerUID;     // proprietário
    uint16_t fib_OwnerGID;
    // extensões
    uint32_t fib_Reserved[3];
} file_info_block_t;

// File open modes (AmigaDOS)
#define MODE_OLDFILE    1005    // abre existente
#define MODE_NEWFILE    1006    // cria novo (sobrescreve)
#define MODE_READWRITE  1004    // leitura+escrita
#define MODE_APPEND     1003    // append
#define MODE_DENY_NONE  0       // sem deny
#define MODE_DENY_READ  1       // nega leitura de outros
#define MODE_DENY_WRITE 2       // nega escrita de outros
#define MODE_DENY_ALL   3       // nega tudo

// Seek offsets
#define OFFSET_BEGINNING  (-1)  // início
#define OFFSET_CURRENT     0    // corrente
#define OFFSET_END         1    // fim
```

### 1.3 Exec Task ( leve, com signals AmigaOS )

```c
// kernel/include/exec/task.h (NOVO)
#pragma once
#include <stdint.h>
#include <list.h>
#include <spinlock.h>

// Signal bits (32-bit bitmask, como AmigaOS)
#define SIGF_HIT         (1L << 0)   // Sinal genérico 0
#define SIGF_ABORT       (1L << 1)   // Abortar task
#define SIGF_USER_2      (1L << 2)
#define SIGF_USER_3      (1L << 3)
#define SIGF_USER_4      (1L << 4)
#define SIGF_USER_5      (1L << 5)
#define SIGF_USER_6      (1L << 6)
#define SIGF_USER_7      (1L << 7)
#define SIGF_FPERR       (1L << 8)   // FPU error
#define SIGF_BREAK       (1L << 15)  // Ctrl+C (igual AmigaOS)
#define SIGF_DOS         (1L << 29)  // DOS operation
#define SIGF_SINGLE      (1L << 30)  // single step
#define SIGF_END_CODE    (1L << 31)  // task terminou

// Task states
#define TASK_STATE_READY     0    // pronto para rodar
#define TASK_STATE_RUNNING   1    // rodando
#define TASK_STATE_WAITING   2    // esperando msgport/signal
#define TASK_STATE_SUSPENDED 3    // suspenso por outra task

typedef struct exec_task {
    // Links
    node_t      et_Node;            // nome + links
    list_head_t et_NodeList;        // na lista do ExecBase

    // Identidade
    char        et_Name[32];
    uint32_t    et_Pid;

    // Sinais (bitmask de 32 bits)
    uint32_t    et_SigRecv;         // bits recebidos
    uint32_t    et_SigWait;         // bits esperados (Wait())
    uint32_t    et_SigExcept;       // exception handler mask
    void      (*et_SigExceptFn)(uint32_t, struct exec_task *);

    // State
    uint32_t    et_State;
    uint32_t    et_Priority;        // -128..+127
    uint32_t    et_BasePri;         // prioridade base (Forbid/Permit)

    // Stack
    void       *et_StackBase;
    uint32_t    etStackSize;

    // Registers (saved on context switch)
    uint64_t    et_Regs[16];        // rax..r15
    uint64_t    et_Rip;
    uint64_t    et_Rsp;
    uint64_t    et_Rflags;
    uint64_t    et_FPU[64];        // fxsave area

    // Memory
    struct mm_struct *et_Mm;        // address space (NULL = shared)

    // IPC
    msgport_t  *et_MsgPort;         // porta padrão da task

    // Timer
    uint64_t    et_WakeTime;        // para Delay()

    // Next/prev para run queue
    struct exec_task *et_Next;
    struct exec_task *et_Prev;
} exec_task_t;
```

---

## Fase 2 — Exec (kernel) — Substitui POSIX process/signal

### 2.1 ExecBase (ponto central)

```c
// kernel/include/exec/exec.h (NOVO)
#pragma once
#include <exec/task.h>
#include <exec/types.h>

typedef struct exec_base {
    uint32_t        eb_Version;
    uint32_t        eb_Revision;

    // Listas
    list_head_t     eb_TaskList;      // todas as tasks
    list_head_t     eb_LibraryList;   // todas as libraries
    list_head_t     eb_InterruptList; // handlers IRQ
    list_head_t     eb_MemList;       // memory regions
    list_head_t     eb_PortList;      // msg ports

    // State
    exec_task_t    *eb_CurrentTask;
    uint32_t        eb_TaskCount;
    uint32_t        eb_LibCount;
    uint64_t        et_TickCount;     // timer ticks desde boot

    // Scheduler
    spinlock_irq_t  eb_SchedLock;
    exec_task_t    *eb_ReadyHead;
    exec_task_t    *eb_ReadyTail;
    uint32_t        eb_PreemptCount;  // 0 = preempt ok, >0 = Forbid()

    // Memory
    uint64_t        eb_MemTotal;
    uint64_t        eb_MemFree;

    // Cpu
    uint32_t        eb_CpuSpeed;      // MHz
    uint8_t         eb_CpuCount;
} exec_base_t;

extern exec_base_t *SysBase;  // ponteiro global

// API do Exec
exec_task_t *exec_create_task(const char *name, int priority,
                               void (*entry)(void), uint32_t stacksize);
void exec_delete_task(exec_task_t *task);
void exec_forbid(void);         // desabilita preempt
void exec_permit(void);         // habilita preempt
void exec_disable(void);        // desabilita interrupts
void exec_enable(void);         // habilita interrupts
void exec_cache_clear(void);
void exec_delay(uint32_t ticks); // 1 tick = 1/100s (PIT 100Hz)
```

### 2.2 Signal (bitmask)

```c
// kernel/include/exec/signal.h (NOVO)
#pragma once
#include <exec/task.h>

// Envia signal bitmask para uma task
void exec_signal(exec_task_t *task, uint32_t mask);

// Espera até que QUALQUER bit do mask seja setado
uint32_t exec_wait(uint32_t mask);

// Espera com timeout (em ticks)
uint32_t exec_wait_timeout(uint32_t mask, uint32_t timeout_ticks);

// Seta bits na task atual
uint32_t exec_set_signal(uint32_t newmask);

// Limpa bits na task atual
uint32_t exec_clear_signal(uint32_t mask);

// Retorna bits recebidos
uint32_t exec_check_signal(exec_task_t *task);
```

**Sem user-stack signal frames.** Ao contrário do POSIX, o AmigaOS não usa `ucontext_t` na stack. O `exec_wait()` simplesmente bloqueia até que `exec_signal()` sete um bit. Quando retorna, o caller lê os bits e age em conformidade.

### 2.3 Libs (library system)

```c
// kernel/include/exec/library.h (NOVO)
#pragma once
#include <exec/types.h>
#include <list.h>

typedef struct library {
    node_t      lib_Node;        // nome na lista
    uint32_t    lib_OpenCnt;     // quantas vezes aberta
    uint32_t    lib_Version;
    uint32_t    lib_Revision;
    uint32_t    lib_Flags;
    const char *lib_IdString;
    void       (*lib_Init)(void);
    void       (*lib_Expunge)(void);
} library_t;

// API
library_t *exec_open_library(const char *name, uint32_t version);
void       exec_close_library(library_t *lib);
library_t *exec_find_library(const char *name);
```

---

## Fase 3 — DOS (filesystem) — Substitui POSIX I/O

### 3.1 File handle (substitui fd)

```c
// kernel/include/dos/doshunks.h (NOVO)
#pragma once

// BPTR = file handle (pointer to internal structure, mas exposto como uint32)
typedef struct dos_filehandle {
    vfs_node_t  *fh_Node;
    uint32_t     fh_Position;
    uint32_t     fh_Flags;
    int32_t      fh_ErrCode;
} dos_filehandle_t;

// AmigaDOS API
BPTR   dos_open(const char *name, int32_t mode);
void   dos_close(BPTR handle);
int32_t dos_read(BPTR handle, void *buffer, int32_t length);
int32_t dos_write(BPTR handle, const void *buffer, int32_t length);
int32_t dos_seek(BPTR handle, int32_t position, int32_t offset_type);
int32_t dos_examine(BPTR handle, file_info_block_t *fib);
int32_t dos_ex_next(BPTR lock, file_info_block_t *fib);
BPTR   dos_lock(const char *name, int32_t type);
void   dos_un_lock(BPTR lock);
int32_t dos_create_dir(const char *name);
int32_t dos_delete_file(const char *name);
int32_t dos_rename(const char *old_name, const char *new_name);
int32_t dos_current_dir(const char *name);
int32_t dos_name_from_lock(BPTR lock, char *name, int32_t len);

// Lock types
#define ACCESS_READ   (-2)
#define ACCESS_WRITE  (-1)

// Std handles (como AmigaOS)
#define dos_input()   (0)   // stdin
#define dos_output()  (1)   // stdout
```

### 3.2 Path parsing

```c
// kernel/include/dos/path.h (NOVO)
#pragma once

char *dos_path_part(const char *path);     // "Work:docs/file.txt" → "file.txt"
char *dos_file_part(const char *path);     // "Work:docs/file.txt" → "file.txt"
char *dos_add_part(const char *dir, const char *file, uint32_t len);
int   dos_parse_pattern(const char *pat, char *buf, uint32_t len);
int   dos_match_pattern(const char *pat, const char *str);

// Wildcards AmigaOS:
//   #?     = qualquer coisa
//   #?.txt = qualquer coisa + .txt
//   ~text  = não contém "text"
```

---

## Fase 4 — Remover POSIX (o que mudar em syscall.c/task.c)

### 4.1 Tabela de syscalls (NOVA)

| # | Nome | O que faz | Substitui |
|---|------|-----------|-----------|
| 0 | `AOS_Exit` | Termina task, limpa imediatamente (sem zombie) | `exit()` |
| 1 | `AOS_CreateTask` | Cria task leve com nome/prioridade/entry/stack | `fork()` + `execve()` |
| 2 | `AOS_Read` | Lê de file handle (BPTR) | `read(fd, buf, len)` |
| 3 | `AOS_Write` | Escreve em file handle (BPTR) | `write(fd, buf, len)` |
| 4 | `AOS_Open` | Abre arquivo com AmigaDOS mode | `open(path, flags)` |
| 5 | `AOS_Close` | Fecha file handle | `close(fd)` |
| 6 | `AOS_Wait` | Espera signal bitmask | `waitpid()` |
| 7 | `AOS_LoadSeg` | Carrega ELF em nova task | `execve()` |
| 8 | `AOS_AllocMem` | Aloca memória (chip/fast/public) | `brk()` + `mmap()` |
| 9 | `AOS_FreeMem` | Libera memória | `munmap()` |
| 10 | `AOS_DoIO` | IOCTL device-specific | `ioctl()` |
| 11 | `AOS_FindTask` | Retorna task atual | `getpid()` |
| 12 | `AOS_Yield` | Yield ao scheduler | `sched_yield()` |
| 13 | `AOS_Delay` | Dorme N ticks (1/100s) | `nanosleep()` |
| 14 | `AOS_GetSysTime` | Retorna ticks desde boot | `gettimeofday()` |
| 15 | `AOS_IoErr` | Último erro | `get_errno()` |
| 16 | `AOS_SetIoErr` | Seta erro | `set_errno()` |
| 17 | `AOS_Signal` | Envia signal bitmask a uma task | `kill()` |
| 18 | `AOS_SetSignal` | Seta bits na task atual | `sigprocmask()` |
| 19 | `AOS_CheckSignal` | Retorna bits recebidos (non-blocking) | — |
| 20 | `AOS_CreateDir` | Cria diretório | `mkdir()` |
| 21 | `AOS_DeleteDir` | Remove diretório | `rmdir()` |
| 22 | `AOS_DeleteFile` | Remove arquivo | `unlink()` |
| 23 | `AOS_Examine` | Preenche FileInfoBlock | `stat()` |
| 24 | `AOS_ExNext` | Próximo entry no dir | `getdents()` |
| 25 | `AOS_Rename` | Renomeia | `rename()` |
| 26 | `AOS_CurrentDir` | Muda CWD | `chdir()` |
| 27 | `AOS_Lock` | Lock em path | — |
| 28 | `AOS_UnLock` | Libera lock | — |
| 29 | `AOS_NameFromLock` | Path do lock | `getcwd()` |
| 30 | `AOS_Flush` | Sync I/O | `fsync()` |
| 31 | `AOS_Seek` | Seek em file handle | `lseek()` |
| 32 | `AOS_PutStr` | Print no kernel log | `syslog()` |
| 33 | `AOS_Assign` | Volume assign | — (já existe) |
| 34 | `AOS_CreatePort` | Cria msg port | — (já existe) |
| 35 | `AOS_DeletePort` | Deleta msg port | — (já existe) |
| 36 | `AOS_PutMsg` | Envia mensagem | — (já existe) |
| 37 | `AOS_GetMsg` | Recebe mensagem | — (já existe) |
| 38 | `AOS_WaitPort` | Espera mensagem | — (já existe) |
| 39 | `AOS_ReplyMsg` | Responde mensagem | — (já existe) |
| 40 | `AOS_Socket` | Cria socket UDP/TCP | — |
| 41 | `AOS_Bind` | Bind socket | — |
| 42 | `AOS_Send` | Envia dados socket | — |
| 43 | `AOS_Recv` | Recebe dados socket | — |
| 44 | `AOS_SigExcept` | Registra exception handler | — |
| 45 | `AOS_SetBasePri` | Muda prioridade | — |
| 46 | `AOS_Forbid` | Desabilita preempt | — |
| 47 | `AOS_Permit` | Habilita preempt | — |
| 48 | `AOS_CacheClear` | Limpa cache | — |
| 49 | `AOS_QueuedIO` | IO assíncrono via msgport | — |
| 50 | `AOS_WaitIO` | Espera IO completion | — |
| 51 | `AOS_AbortIO` | Cancela IO | — |
| 52 | `AOS_IntVector` | Registra interrupt handler | — |
| 53 | `AOS_RemInt` | Remove interrupt handler | — |

### 4.2 O que REMOVER do task.c

| Item | Linha | Ação |
|------|-------|------|
| `sigaction_t` struct | task.h:61-67 | **REMOVER** — não há handlers POSIX |
| `kernel_sigset_t` | task.h:73-75 | **REMOVER** — usar uint32_t direto |
| `ucontext_t` | task.h:82-91 | **REMOVER** — sem frames na user stack |
| `SIG_DFL`, `SIG_IGN`, `SIG_ERR` | task.h:49-51 | **REMOVER** — bitmask puro |
| `SA_*` flags | task.h:53-59 | **REMOVER** |
| `SIG_BLOCK/UNBLOCK/SETMASK` | task.h:77-79 | **REMOVER** — usar SetSignal |
| `sigaction[32]` no task_struct | task.h | **REMOVER** — não há handlers |
| `blocked`, `pending` no task_struct | task.h | **SUBSTITUIR** por `et_SigRecv`, `et_SigWait` |
| `send_sig()` | task.c:376-402 | **SUBSTITUIR** por `exec_signal()` |
| `force_sig()` | task.c:404-406 | **SUBSTITUIR** por `exec_signal()` forçado |
| `do_signal()` | task.c:409-521 | **REMOVER** — sem dispatcher de handler |
| `aos_signal()` | task.c:524-539 | **SUBSTITUIR** por `exec_set_signal()` |
| `aos_setsignal()` | task.c:541-575 | **SUBSTITUIR** por `exec_set_signal()` |
| `aos_return_signal()` | task.c:577-632 | **REMOVER** — sem sigreturn |
| `aos_send_signal()` | task.c:634-648 | **SUBSTITUIR** por `exec_signal()` por task ptr |
| 31 signal numbers (SIGHUP..SIGSYS) | task.h:16-47 | **REMOVER** — usar bitmask bits |
| `TASK_STATE_ZOMBIE` | task.h:98 | **REMOVER** — cleanup imediato |
| `TASK_STATE_STOPPED` | task.h:97 | **SUBSTITUIR** por SUSPENDED |
| `pgid`, `sid` | task.h:171-172 | **REMOVER** — sem process groups |
| `wait_chldexit` | task.h:154 | **REMOVER** — usar Wait() signal |

### 4.3 O que REMOVER do syscall.c

| Item | Linha | Ação |
|------|-------|------|
| `aos_spawn_task()` (fork) | syscall.c:115-180 | **REMOVER** — usar CreateTask |
| `aos_loadseg()` (execve) | syscall.c:683-822 | **REESCREVER** sem argv/envp POSIX |
| `aos_wait()` (waitpid) | syscall.c:199-294 | **REMOVER** — usar Wait() bitmask |
| `aos_pipe()` | syscall.c:939-989 | **REMOVER** — usar MsgPorts |
| `aos_setbrk()` | syscall.c:828-867 | **REMOVER** — usar AllocMem |
| `aos_allocmem()` (mmap) | syscall.c:869-886 | **REESCREVER** com MEMF flags |
| `aos_freemem()` (munmap) | syscall.c:888-914 | **REESCREVER** sem POSIX semantics |
| `aos_set_procgroup()` | syscall.c:317 | **REMOVER** |
| `aos_get_procgroup()` | syscall.c:351 | **REMOVER** |
| `aos_set_conproc()` | syscall.c:375 | **REMOVER** |
| `aos_get_conproc()` | syscall.c:402 | **REMOVER** |
| `aos_socket()` | syscall.c:1303 | **REESCREVER** bsd.socket.library |
| `aos_bind()` | syscall.c:1321 | **REESCREVER** |
| `aos_send()` | syscall.c:1336 | **REESCREVER** |
| `aos_recv()` | syscall.c:1352 | **REESCREVER** |

### 4.4 O que REMOVER do syscall.h

| Item | Linha | Ação |
|------|-------|------|
| `struct stat` | syscall.h:68-82 | **REMOVER** — usar FileInfoBlock |
| `struct dirent` | syscall.h:85-91 | **REMOVER** — usar ExNext |
| POSIX open flags (O_RDONLY..) | syscall.h:52-66 | **REMOVER** — usar MODE_* |
| POSIX errno constants | syscall.h:16-50 | **REMOVER** — usar AOS_ERR_* |
| `struct timespec` | syscall.h:10-13 | **REMOVER** — usar ticks |
| `struct sockaddr/sockaddr_in` | syscall.h:94-104 | **REMOVER** — usar AmigaOS format |
| `AF_INET`, `SOCK_*` | syscall.h:106-108 | **REMOVER** |
| `SYS_*` aliases | syscall.h | **REMOVER** — só manter `AOS_*` |

---

## Fase 5 — O que MANTER (já é moderno e útil)

| Feature | Por quê manter |
|---------|----------------|
| MMU com per-process PML4 | Segurança, GPU, IOMMU |
| Preemptive round-robin scheduler | Essencial |
| PIT 100Hz + timer ticks | Base do Delay() |
| MsgPorts (6 syscalls) | Já AmigaOS-like |
| Assigns | Já AmigaOS-like |
| DRM/GPU driver framework | Modernidade |
| VFS + RamFS + FAT32 | Filesystem real |
| ProcFS | Debug |
| DevFS | Device nodes |
| Kernel heap (kmalloc/kfree) | Interno |
| PMM bitmap | Interno |
| VMM 4-level paging | Interno |
| PCI enumeration | Interno |
| IRQ handlers (shared chain) | Interno |
| DMA fence/resv | GPU sync |
| Block cache (bcache) | Performance |

---

## Fase 6 — dogin (shell) — Atualizar APIs

### O que muda no dogin

| Comando | Antes (POSIX) | Depois (AmigaOS) |
|---------|---------------|-------------------|
| `List` | `vfs_readdir()` | `ExNext()` + `FileInfoBlock` |
| `Type` | `vfs_read()` | `dos_read()` |
| `Copy` | `vfs_read()`+`vfs_write()` | `dos_read()`+`dos_write()` |
| `Delete` | `vfs_unlink()` | `dos_delete_file()` |
| `MakeDir` | `vfs_mkdir()` | `dos_create_dir()` |
| `Cd` | `assign_set()` | `dos_current_dir()` |
| `Run` | `task_create_user()` | `AOS_CreateTask()` + `AOS_LoadSeg()` |
| `Status` | `task_list` walk | `exec_find_task()` |
| `Avail` | `pmm_get_free_memory()` | `SysBase->eb_MemFree` |
| `Version` | hardcoded string | `SysBase->eb_Version` |
| Prompt | `Work:>` | `Work:>` (mantém) |

### Novos comandos dogin

| Comando | Função |
|---------|--------|
| `Which` | Encontra comando em `C:` |
| `Path` | Mostra/adiciona path de busca |
| `Set` | Define variável de ambiente |
| `Evaluate` | Calcula expressão (`Eval "2+2"`) |
| `If`/`Then`/`Else`/`EndIf` | Condicional |
| `While`/`EndWhile` | Loop |
| `Run`/`Wait` | Background + wait |
| `Break` | Interrompe loop |

---

## Ordem de Implementação

```
Semana 1:  exec/types.h + exec/task.h + exec/exec.h + exec/signal.h
           → Criar todos os novos tipos/structs

Semana 2:  exec_create_task() + exec_delete_task() + exec_signal() + exec_wait()
           → Implementar kernel sem tocar em syscalls existentes

Semana 3:  exec library system + SysBase init no boot
           → Integrar ao kernel.c

Semana 4:  dos/types.h + dos/dos.h + dos/path.h
           → FileInfoBlock, open modes, path parsing

Semana 5:  dos_open/close/read/write/seek/examine
           → File handle system

Semana 6:  Atualizar dogin para usar novas APIs
           → Remover dependências de fd_table em dogin.c

Semana 7:  Atualizar syscall.h — remover tipos/flags POSIX
           → Novos AOS_* syscall numbers

Semana 8:  Atualizar task.c — remover sigaction/do_signal/posix signals
           → Implementar exec_signal/wait no kernel

Semana 9:  Atualizar syscall.c — remover fork/pipe/brk/mmap/setpgid
           → Redirecionar para CreateTask/AllocMem

Semana 10: Build + testes em QEMU + vGPU
           → Boot OK, dogin funcional com novas APIs
```

---

## Riscos

| Risco | Mitigação |
|-------|-----------|
| Quebra GPU port (usa mmap/mprotect) | Manter internamente; expor via AOS_AllocMem |
| Quebra drivers existentes | Compat layer: `static inline` wrappers |
| Dogin depende de TTY/fd | Refatorar dogin para usar BPTR handles |
| FAT32 driver usa vfs_read/write | VFS mantido internamente; DOS layer é wrapper |
| Tempo de implementação | 10 semanas para migração completa |

---

## Referência: Mapeamento POSIX → AmigaOS

| POSIX | AmigaOS | Diferença |
|-------|---------|-----------|
| `fork()` | `CreateTask()` | Sem COW fork; tasks são independentes |
| `execve(path, argv, envp)` | `LoadSeg()` | Sem argv/envp; tags ou structs |
| `exit(code)` | `return` | Sem exit code; signal SIGF_END_CODE |
| `waitpid(pid, &status, opts)` | `Wait(SIGF_END_CODE)` | Bitmask, não PID |
| `signal(sig, handler)` | `SetSignal(mask)` | Bitmask, sem handlers |
| `kill(pid, sig)` | `Signal(task, mask)` | Por task pointer |
| `open(path, flags, mode)` | `Open(path, mode)` | AmigaDOS modes |
| `read(fd, buf, len)` | `Read(handle, buf, len)` | BPTR, não fd int |
| `write(fd, buf, len)` | `Write(handle, buf, len)` | BPTR |
| `close(fd)` | `Close(handle)` | BPTR |
| `stat(path, &st)` | `Examine(lock, &fib)` | FileInfoBlock |
| `getdents(fd, ...)` | `ExNext(lock, &fib)` | Iterative |
| `mmap(...)` | `AllocMem(size, flags)` | MEMF_CHIP/FAST/PUBLIC |
| `munmap(...)` | `FreeMem(ptr, size)` | Precisa do size |
| `nanosleep(ts)` | `Delay(ticks)` | Ticks, não nanoseconds |
| `gettimeofday(&tv)` | `DateStamp(&ds)` | AmigaDOS format |
| `pipe(fds[2])` | `CreatePort()` + messages | MsgPorts |
| `ioctl(fd, ...)` | `DoIO()` device-specific | Por device |

---

*Documento vivo — atualizar a cada fase concluída.*
