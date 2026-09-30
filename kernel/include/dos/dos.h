#ifndef DOS_DOS_H
#define DOS_DOS_H

#include <stdint.h>
#include <dos/types.h>

/* Forward declaration */
struct vfs_node;
typedef struct vfs_node vfs_node_t;
struct file;
typedef struct file file_t;

/* Size of FileName buffer in FileInfoBlock */
#define DOS_FILENAMESIZE 108
#define DOS_COMMENTSIZE   80

/*
 * FileInfoBlock — equivalent of AmigaOS struct FileInfoBlock.
 * Returned by Examine()/ExNext().
 */
typedef struct file_info_block {
    int32_t  fib_DiskKey;         /* internal key (inode number) */
    int32_t  fib_DirEntryType;    /* >0 = directory, <0 = file */
    char     fib_FileName[DOS_FILENAMESIZE];
    int32_t  fib_Protection;      /* protection bits (AOS_FIBF_*) */
    int32_t  fib_EntryType;
    int32_t  fib_Size;            /* size in bytes */
    int32_t  fib_NumBlocks;       /* allocated blocks */
    int8_t   fib_Date[12];        /* last modification date */
    char     fib_Comment[DOS_COMMENTSIZE];
    uint16_t fib_OwnerUID;
    uint16_t fib_OwnerGID;
    uint32_t fib_Reserved[3];
} file_info_block_t;

/* BPTR = file handle (opaque to userspace) */
typedef struct dos_handle {
    vfs_node_t  *dh_Node;
    file_t      *dh_File;
    uint32_t     dh_Position;
    uint32_t     dh_Flags;
    int32_t      dh_ErrCode;
    uint64_t     dh_Owner;   /* pid of owning task (0 = free/reserved) */
} dos_handle_t;

struct task_struct;

/* Close all DOS handles owned by @t (called from task_exit). */
void dos_task_cleanup(struct task_struct *t);
uint64_t dos_handle_owner(BPTR handle);

/* Standard I/O */
BPTR  dos_input(void);
BPTR  dos_output(void);

/* File operations */
BPTR    dos_open(const char *name, int32_t mode);
void    dos_close(BPTR handle);
int32_t dos_read(BPTR handle, void *buffer, int32_t length);
int32_t dos_write(BPTR handle, const void *buffer, int32_t length);
int32_t dos_seek(BPTR handle, int32_t position, int32_t offset_type);
int32_t dos_flush(BPTR handle);
int32_t dos_do_io(BPTR handle, uint64_t request, void *arg);

/* Directory operations */
int32_t dos_examine(BPTR lock, file_info_block_t *fib);
int32_t dos_ex_next(BPTR lock, file_info_block_t *fib);
BPTR    dos_lock(const char *name, int32_t lock_type);
void    dos_un_lock(BPTR lock);
int32_t dos_create_dir(const char *name);
int32_t dos_delete_file(const char *name);
int32_t dos_rename(const char *old_name, const char *new_name);
int32_t dos_current_dir(const char *name);
int32_t dos_name_from_lock(BPTR lock, char *name, int32_t len);

/* Error handling */
int32_t dos_io_err(void);
void    dos_set_io_err(int32_t err);

/* Initialization */
void dos_init(void);

#endif
