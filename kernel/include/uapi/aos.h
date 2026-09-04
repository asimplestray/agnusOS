/* uapi/aos.h — ABI traps AmigaOS-like (uAPI 1.0)
 *
 * Trap numbers congelados em c32c427 (AOS_0..52). Numeric ABI idêntico ao
 * antigo SYS_* Linux para compat, mas nomes seguem Exec/DOS.
 * Regra: nunca reutilizar número; novos traps em 53+.
 * Dispacho via int 0x80 / syscall (kernel/syscall.c:34) com frame.
 *
 * Versão: 1.0 — tag uapi-1.0
 */

#ifndef _UAPI_AOS_H_
#define _UAPI_AOS_H_

#include <stdint.h>
#include <stddef.h>

#define AOS_UAPI_VERSION_MAJOR 1
#define AOS_UAPI_VERSION_MINOR 0

/* 0-45 — base Unix renomeada Amiga */
#define AOS_Exit            0
#define AOS_SpawnTask       1
#define AOS_Read            2
#define AOS_Write           3
#define AOS_Open            4
#define AOS_Close           5
#define AOS_Wait            6
#define AOS_LoadSeg         7
#define AOS_SetBrk          8
#define AOS_AllocMem        9
#define AOS_FreeMem        10
#define AOS_DoIO           11
#define AOS_FindTask       12
#define AOS_Yield          13
#define AOS_Delay          14
#define AOS_GetSysTime     15
#define AOS_AddTask        16
#define AOS_Signal         17
#define AOS_SetSignal      18
#define AOS_ReturnSignal   19
#define AOS_SendSignal     20
#define AOS_Pipe           21
#define AOS_Seek           22
#define AOS_Examine        23
#define AOS_Clock          24
#define AOS_PutStr         25
#define AOS_IoErr          26
#define AOS_SetIoErr       27
#define AOS_SetProcGroup   28
#define AOS_GetProcGroup   29
#define AOS_SetConProc     30
#define AOS_GetConProc     31
#define AOS_CreateDir      32
#define AOS_DeleteDir      33
#define AOS_DeleteFile     34
#define AOS_CurrentDir     35
#define AOS_CurrentDirFD   36
#define AOS_LockCWD        37
#define AOS_Rename         38
#define AOS_ExamineDir     39
#define AOS_Socket         40
#define AOS_Bind           41
#define AOS_Send           42
#define AOS_Recv           43
#define AOS_CloseSocket    44
#define AOS_Flush          45
/* f9b3068 Amiga extentions */
#define AOS_Assign         46
#define AOS_CreatePort     47
#define AOS_DeletePort     48
#define AOS_PutMsg         49
#define AOS_GetMsg         50
#define AOS_WaitPort       51
#define AOS_ReplyMsg       52

#define AOS_NR_SYSCALLS    53

/* Compat aliases — código antigo ainda compila com SYS_* */
#define SYS_EXIT        AOS_Exit
#define SYS_FORK        AOS_SpawnTask
#define SYS_READ        AOS_Read
#define SYS_WRITE       AOS_Write
#define SYS_OPEN        AOS_Open
#define SYS_CLOSE       AOS_Close
#define SYS_WAITPID     AOS_Wait
#define SYS_EXECVE      AOS_LoadSeg
#define SYS_BRK         AOS_SetBrk
#define SYS_MMAP        AOS_AllocMem
#define SYS_MUNMAP      AOS_FreeMem
#define SYS_IOCTL       AOS_DoIO
#define SYS_GETPID      AOS_FindTask

/* Flags — compat kernel/include/syscall.h:52 */
#define AOS_O_RDONLY    0x0000
#define AOS_O_WRONLY    0x0001
#define AOS_O_RDWR      0x0002
#define AOS_O_CREAT     0x0040
#define AOS_O_TRUNC     0x0200

/* Assigns (kernel/fs/assign.c:1) */
#define AOS_ASSIGN_SET    0
#define AOS_ASSIGN_UNSET  1
#define AOS_ASSIGN_GET    2
#define ASSIGN_MAX_NAME  32
#define ASSIGN_MAX_PATH 256

/* MsgPort (kernel/ipc/msgport.c:1) — inline payload 128B */
#define AOS_MSG_MAX_PAYLOAD 128

struct aos_msg {
    uint32_t size;
    uint32_t code;
    int32_t  reply_port;
    char     payload[AOS_MSG_MAX_PAYLOAD];
};

/* stat compat — kernel/include/syscall.h:67 */
struct aos_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_mode;
    uint64_t st_size;
};

#endif /* _UAPI_AOS_H_ */
