/*
 * VFS — Virtual File System dispatch layer
 *
 * All file operations are routed through the vfs_node_t function pointer
 * table.  A single global mount point (vfs_root) keeps things simple for
 * now; multi-mount support can be added later.
 */

#include <vfs.h>
#include <ramfs.h>
#include <assign.h>
#include <screen.h>
#include <stddef.h>
#include <stdint.h>

vfs_node_t *vfs_root = NULL;

/* -------------------------------------------------------------------------
 * Low-level dispatch helpers
 * ---------------------------------------------------------------------- */

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    if (node && node->read)
        return node->read(node, offset, size, buf);
    return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    if (node && node->write)
        return node->write(node, offset, size, buf);
    return 0;
}

void vfs_open(vfs_node_t *node)
{
    if (node && node->open)
        node->open(node);
}

void vfs_close(vfs_node_t *node)
{
    if (node && node->close)
        node->close(node);
}

vfs_dirent_t *vfs_readdir(vfs_node_t *node, uint32_t index)
{
    if (node && (node->flags & VFS_DIRECTORY) && node->readdir)
        return node->readdir(node, index);
    return NULL;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name)
{
    if (node && (node->flags & VFS_DIRECTORY) && node->finddir)
        return node->finddir(node, name);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Directory operations
 * ---------------------------------------------------------------------- */

int vfs_mkdir(vfs_node_t *parent, const char *name, uint32_t mode)
{
    (void)mode;
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !parent->mkdir)
        return -ENOSYS;
    return parent->mkdir(parent, name, mode);
}

int vfs_rmdir(vfs_node_t *parent, const char *name)
{
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !parent->rmdir)
        return -ENOSYS;
    return parent->rmdir(parent, name);
}

int vfs_unlink(vfs_node_t *parent, const char *name)
{
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !parent->unlink)
        return -ENOSYS;
    return parent->unlink(parent, name);
}

int vfs_rename(vfs_node_t *old_parent, const char *old_name, vfs_node_t *new_parent, const char *new_name)
{
    if (!old_parent || !(old_parent->flags & VFS_DIRECTORY) || !old_parent->rename)
        return -ENOSYS;
    return old_parent->rename(old_parent, old_name, new_parent, new_name);
}

/* -------------------------------------------------------------------------
 * Path resolution
 *
 * Handles absolute paths only.  Components are split on '/'.
 * Example: "/bin/hello" → root → find "bin" → find "hello"
 * ---------------------------------------------------------------------- */
vfs_node_t *vfs_resolve(const char *path)
{
    if (!vfs_root || !path)
        return NULL;

    if (path[0] != '/') {
        /* Volume-style path ("Sys:bin/hello"): expand through assigns */
        static char expanded[ASSIGN_MAX_PATH];
        if (assign_expand(path, expanded, sizeof expanded) != 0)
            return NULL;
        return vfs_resolve(expanded);
    }

    vfs_node_t *node = vfs_root;

    /* Skip leading slash */
    path++;

    /* Iterate path components */
    static char component[VFS_MAX_NAME];
    while (*path) {
        /* Extract next component */
        int i = 0;
        while (*path && *path != '/' && i < (int)(VFS_MAX_NAME - 1))
            component[i++] = *path++;
        component[i] = '\0';

        /* Skip consecutive slashes */
        while (*path == '/')
            path++;

        if (i == 0)
            continue; /* Empty component after trailing slash */

        node = vfs_finddir(node, component);
        if (!node)
            return NULL;
    }

    return node;
}

/* -------------------------------------------------------------------------
 * Mount / init
 * ---------------------------------------------------------------------- */

void vfs_mount(vfs_node_t *root)
{
    vfs_root = root;
}

void vfs_init(void)
{
    vfs_node_t *root = ramfs_init();
    if (!root) {
        screen_log("FAIL", COLOR_LIGHT_RED, "RamFS init falhou!");
        return;
    }
    vfs_mount(root);
    screen_log("OK", COLOR_LIGHT_GREEN, "VFS + RamFS montado em /.");
}
