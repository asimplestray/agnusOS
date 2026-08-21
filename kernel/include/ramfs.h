#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>
#include <vfs.h>

/* Maximum children per directory node */
#define RAMFS_MAX_CHILDREN  64

typedef struct ramfs_node {
    vfs_node_t      vfs;                          /* MUST be first */
    uint8_t        *data;                          /* File data pointer */
    uint32_t        data_size;                     /* Allocated data size */
    struct ramfs_node *children[RAMFS_MAX_CHILDREN];
    uint32_t        num_children;
} ramfs_node_t;

/* Create a new ramfs directory node (not yet attached to the tree) */
ramfs_node_t *ramfs_mkdir_node(const char *name);

/* Create a new ramfs file node with static content */
ramfs_node_t *ramfs_mkfile_node(const char *name, const uint8_t *data, uint32_t size);

/* Attach a child node to a directory node */
void ramfs_attach(ramfs_node_t *dir, ramfs_node_t *child);

/* Look up a child of a ramfs directory by name (exact match) */
vfs_node_t *ramfs_finddir_node(ramfs_node_t *dir, const char *name);

/* Return the i-th child of a ramfs directory, or NULL */
vfs_node_t *ramfs_readdir_node(ramfs_node_t *dir, uint32_t index);

/* Initialize the RamFS, build the initial tree, return the root vfs_node_t */
vfs_node_t *ramfs_init(void);

#endif
