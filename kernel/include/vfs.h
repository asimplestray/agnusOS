#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* Errno codes used by VFS (guarded for multi-include) */
#ifndef ENOENT
#define ENOENT          2
#endif
#ifndef EIO
#define EIO             5
#endif
#ifndef EBADF
#define EBADF           9
#endif
#ifndef ENOMEM
#define ENOMEM          12
#endif
#ifndef EACCES
#define EACCES          13
#endif
#ifndef EEXIST
#define EEXIST          17
#endif
#ifndef ENOTDIR
#define ENOTDIR         20
#endif
#ifndef EISDIR
#define EISDIR          21
#endif
#ifndef EINVAL
#define EINVAL          22
#endif
#ifndef ENOSPC
#define ENOSPC          28
#endif
#ifndef ENOSYS
#define ENOSYS          38
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY       39
#endif

/* Node type flags */
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_SYMLINK     0x04
#define VFS_PIPE        0x08
#define VFS_CHARDEVICE  0x10
#define VFS_BLOCKDEVICE 0x20

#define VFS_MAX_NAME    128

struct vfs_node;

typedef uint32_t (*vfs_read_fn) (struct vfs_node *, uint32_t offset, uint32_t size, uint8_t *buf);
typedef uint32_t (*vfs_write_fn)(struct vfs_node *, uint32_t offset, uint32_t size, const uint8_t *buf);
typedef void     (*vfs_open_fn) (struct vfs_node *);
typedef void     (*vfs_close_fn)(struct vfs_node *);
typedef struct vfs_dirent *(*vfs_readdir_fn)(struct vfs_node *, uint32_t index);
typedef struct vfs_node   *(*vfs_finddir_fn)(struct vfs_node *, const char *name);

typedef struct vfs_node {
    char     name[VFS_MAX_NAME];
    uint32_t flags;     /* VFS_FILE | VFS_DIRECTORY | ... */
    uint32_t inode;     /* Unique node ID */
    uint32_t length;    /* File size in bytes (0 for dirs) */
    uint32_t uid, gid;

    vfs_read_fn    read;
    vfs_write_fn   write;
    vfs_open_fn    open;
    vfs_close_fn   close;
    vfs_readdir_fn readdir;
    vfs_finddir_fn finddir;
    int          (*mkdir)(struct vfs_node *parent, const char *name, uint32_t mode);
    int          (*rmdir)(struct vfs_node *parent, const char *name);
    int          (*unlink)(struct vfs_node *parent, const char *name);
    int          (*rename)(struct vfs_node *old_parent, const char *old_name, struct vfs_node *new_parent, const char *new_name);

    struct vfs_node *ptr; /* Symlink/mountpoint target */
} vfs_node_t;

typedef struct vfs_dirent {
    char     name[VFS_MAX_NAME];
    uint32_t inode;
} vfs_dirent_t;

/* Global VFS root */
extern vfs_node_t *vfs_root;

/* VFS operations (dispatch through node function pointers) */
uint32_t    vfs_read   (vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf);
uint32_t    vfs_write  (vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf);
void        vfs_open   (vfs_node_t *node);
void        vfs_close  (vfs_node_t *node);
vfs_dirent_t *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t   *vfs_finddir(vfs_node_t *node, const char *name);

/* Directory operations */
int         vfs_mkdir  (vfs_node_t *parent, const char *name, uint32_t mode);
int         vfs_rmdir  (vfs_node_t *parent, const char *name);
int         vfs_unlink (vfs_node_t *parent, const char *name);
int         vfs_rename (vfs_node_t *old_parent, const char *old_name, vfs_node_t *new_parent, const char *new_name);

/* Resolve an absolute path to a node (returns NULL if not found) */
vfs_node_t *vfs_resolve(const char *path);

/* Mount a node at the VFS root (simple, single-mount implementation) */
void vfs_mount(vfs_node_t *root);

/* Initialize VFS and populate with RamFS */
void vfs_init(void);

/* Pipe support */
vfs_node_t *pipe_create(vfs_node_t **write_end);

#endif
