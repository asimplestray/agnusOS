/*
 * RamFS — In-memory filesystem implementation
 *
 * Each node is a ramfs_node_t (a vfs_node_t with an embedded child array
 * and a data pointer).  All allocations use the kernel heap (kmalloc/kfree).
 *
 * The initial tree is populated inside ramfs_init():
 *
 *   /           (directory)
 *   ├── bin/    (directory)
 *   │   └── hello   (ELF or raw binary — placeholder text for now)
 *   └── etc/    (directory)
 *       └── version (text file)
 */

#include <ramfs.h>
#include <vfs.h>
#include <kheap.h>
#include <screen.h>
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* -------------------------------------------------------------------------
 * VFS operation implementations for RamFS nodes
 * ---------------------------------------------------------------------- */

static uint32_t ramfs_file_read(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buf)
{
    ramfs_node_t *rn = (ramfs_node_t *)node;
    if (!rn->data || offset >= rn->data_size) return 0;
    uint32_t avail = rn->data_size - offset;
    if (size > avail) size = avail;
    mem_copy(buf, rn->data + offset, size);
    return size;
}

static uint32_t ramfs_file_write(vfs_node_t *node, uint32_t offset,
                                 uint32_t size, const uint8_t *buf)
{
    ramfs_node_t *rn = (ramfs_node_t *)node;
    if (!rn->data || offset + size > rn->data_size) return 0;
    mem_copy(rn->data + offset, buf, size);
    return size;
}

static vfs_dirent_t *ramfs_dir_readdir(vfs_node_t *node, uint32_t index)
{
    ramfs_node_t *rn = (ramfs_node_t *)node;
    if (index >= rn->num_children) return NULL;
    static vfs_dirent_t de;
    str_copy(de.name, rn->children[index]->vfs.name, VFS_MAX_NAME);
    de.inode = rn->children[index]->vfs.inode;
    return &de;
}

static vfs_node_t *ramfs_dir_finddir(vfs_node_t *node, const char *name)
{
    ramfs_node_t *rn = (ramfs_node_t *)node;
    for (uint32_t i = 0; i < rn->num_children; i++) {
        if (str_eq(rn->children[i]->vfs.name, name))
            return &rn->children[i]->vfs;
    }
    return NULL;
}

static int ramfs_dir_mkdir(vfs_node_t *parent, const char *name, uint32_t mode)
{
    (void)mode;
    ramfs_node_t *rn = (ramfs_node_t *)parent;
    if (rn->num_children >= RAMFS_MAX_CHILDREN)
        return -ENOSPC;

    if (ramfs_dir_finddir(parent, name))
        return -EEXIST;

    ramfs_node_t *child = ramfs_mkdir_node(name);
    if (!child)
        return -ENOMEM;

    ramfs_attach(rn, child);
    return 0;
}

static int ramfs_dir_rmdir(vfs_node_t *parent, const char *name)
{
    ramfs_node_t *rn = (ramfs_node_t *)parent;

    for (uint32_t i = 0; i < rn->num_children; i++) {
        if (str_eq(rn->children[i]->vfs.name, name)) {
            if (!(rn->children[i]->vfs.flags & VFS_DIRECTORY))
                return -ENOTDIR;
            if (rn->children[i]->num_children > 0)
                return -ENOTEMPTY;

            /* Free the child node */
            kfree(rn->children[i]);

            /* Shift remaining children */
            for (uint32_t j = i; j < rn->num_children - 1; j++)
                rn->children[j] = rn->children[j + 1];
            rn->num_children--;
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_dir_unlink(vfs_node_t *parent, const char *name)
{
    ramfs_node_t *rn = (ramfs_node_t *)parent;

    for (uint32_t i = 0; i < rn->num_children; i++) {
        if (str_eq(rn->children[i]->vfs.name, name)) {
            if (rn->children[i]->vfs.flags & VFS_DIRECTORY)
                return -EISDIR;

            /* Free file data */
            if (rn->children[i]->data)
                kfree(rn->children[i]->data);

            /* Free the child node */
            kfree(rn->children[i]);

            /* Shift remaining children */
            for (uint32_t j = i; j < rn->num_children - 1; j++)
                rn->children[j] = rn->children[j + 1];
            rn->num_children--;
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_dir_rename(vfs_node_t *old_parent, const char *old_name,
                            vfs_node_t *new_parent, const char *new_name)
{
    ramfs_node_t *old_rn = (ramfs_node_t *)old_parent;
    ramfs_node_t *new_rn = (ramfs_node_t *)new_parent;

    int old_idx = -1;
    for (uint32_t i = 0; i < old_rn->num_children; i++) {
        if (str_eq(old_rn->children[i]->vfs.name, old_name)) {
            old_idx = i;
            break;
        }
    }
    if (old_idx < 0)
        return -ENOENT;

    if (ramfs_dir_finddir(new_parent, new_name))
        return -EEXIST;

    if (new_rn->num_children >= RAMFS_MAX_CHILDREN)
        return -ENOSPC;

    /* Move the child */
    ramfs_node_t *child = old_rn->children[old_idx];

    /* Shift remaining children in old parent */
    for (uint32_t j = old_idx; j < old_rn->num_children - 1; j++)
        old_rn->children[j] = old_rn->children[j + 1];
    old_rn->num_children--;

    /* Add to new parent */
    str_copy(child->vfs.name, new_name, VFS_MAX_NAME);
    ramfs_attach(new_rn, child);

    return 0;
}

/* -------------------------------------------------------------------------
 * Node constructors
 * ---------------------------------------------------------------------- */

static uint32_t next_inode = 1;

ramfs_node_t *ramfs_mkdir_node(const char *name)
{
    ramfs_node_t *rn = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (!rn) return NULL;
    mem_zero(rn, sizeof(ramfs_node_t));

    str_copy(rn->vfs.name, name, VFS_MAX_NAME);
    rn->vfs.flags     = VFS_DIRECTORY;
    rn->vfs.inode     = next_inode++;
    rn->vfs.length    = 0;
    rn->vfs.readdir   = ramfs_dir_readdir;
    rn->vfs.finddir   = ramfs_dir_finddir;
    rn->vfs.mkdir     = ramfs_dir_mkdir;
    rn->vfs.rmdir     = ramfs_dir_rmdir;
    rn->vfs.unlink    = ramfs_dir_unlink;
    rn->vfs.rename    = ramfs_dir_rename;

    return rn;
}

ramfs_node_t *ramfs_mkfile_node(const char *name, const uint8_t *data, uint32_t size)
{
    ramfs_node_t *rn = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (!rn) return NULL;
    mem_zero(rn, sizeof(ramfs_node_t));

    str_copy(rn->vfs.name, name, VFS_MAX_NAME);
    rn->vfs.flags     = VFS_FILE;
    rn->vfs.inode     = next_inode++;
    rn->vfs.length    = size;
    rn->vfs.read      = ramfs_file_read;
    rn->vfs.write     = ramfs_file_write;

    if (size > 0 && data) {
        rn->data = (uint8_t *)kmalloc(size);
        if (rn->data) {
            mem_copy(rn->data, data, size);
            rn->data_size = size;
        }
    }

    return rn;
}

void ramfs_attach(ramfs_node_t *dir, ramfs_node_t *child)
{
    if (!dir || !child) return;
    if (dir->num_children >= RAMFS_MAX_CHILDREN) return;
    dir->children[dir->num_children++] = child;
}

vfs_node_t *ramfs_finddir_node(ramfs_node_t *dir, const char *name)
{
    if (!dir || !name) return NULL;
    for (uint32_t i = 0; i < dir->num_children; i++) {
        if (str_eq(dir->children[i]->vfs.name, name))
            return &dir->children[i]->vfs;
    }
    return NULL;
}

vfs_node_t *ramfs_readdir_node(ramfs_node_t *dir, uint32_t index)
{
    if (!dir || index >= dir->num_children) return NULL;
    return &dir->children[index]->vfs;
}

/* -------------------------------------------------------------------------
 * Initial tree population
 *
 * /etc/version  — simple text file
 * /bin/         — empty for now; kernel can add ELFs at runtime
 * ---------------------------------------------------------------------- */

/* A minimal ELF64 "hello world" binary that calls sys_write(1,...) then
 * sys_exit(0) via int 0x80.  This is hand-assembled machine code so that
 * the kernel can run a real Ring-3 process without needing a userspace
 * compiler or initrd.
 *
 * Syscall numbers (ApolloOS):
 *   SYS_WRITE = 3   (fd=1, buf=ptr, count=len)
 *   SYS_EXIT  = 0   (code=0)
 *
 * The binary is a flat ELF64 executable loaded at virtual address 0x400000.
 * It has a single PT_LOAD segment covering the whole image.
 *
 * Machine code (text section, relative offset 0x78 from file start):
 *   mov rax, 3          ; SYS_WRITE
 *   mov rdi, 1          ; fd = stdout
 *   lea rsi, [rel msg]  ; buf
 *   mov rdx, 14         ; len
 *   int 0x80
 *   mov rax, 0          ; SYS_EXIT
 *   xor rdi, rdi
 *   int 0x80
 *   .loop: jmp .loop
 *   msg: db "Ola do Ring 3!", 10
 */
static const uint8_t hello_elf[] = {
    0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x3e, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x78, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x38, 0x00, 0x01, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xb8, 0x03, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0xbe,
    0x53, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba, 0x27, 0x00, 0x00,
    0x00, 0xcd, 0x80, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00,
    0x00, 0x48, 0xbe, 0x77, 0x04, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba,
    0x3f, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x48, 0x89, 0x04, 0x25, 0x57, 0x03,
    0x40, 0x00, 0xb8, 0x03, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00,
    0x48, 0xbe, 0x7a, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba, 0x05,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x03, 0x00, 0x00, 0x00, 0xbf, 0x01,
    0x00, 0x00, 0x00, 0x48, 0xbe, 0x77, 0x04, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x48, 0x8b, 0x14, 0x25, 0x57, 0x03, 0x40, 0x00, 0xcd, 0x80, 0xb8,
    0x03, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0xbe, 0x7f,
    0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba, 0x23, 0x00, 0x00, 0x00,
    0xcd, 0x80, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x48, 0xbf, 0xa2, 0x02, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xbe, 0x00, 0x00, 0x00, 0x00, 0xba, 0x00,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0x48, 0x89, 0x04, 0x25, 0x5f, 0x03, 0x40,
    0x00, 0x48, 0x83, 0xf8, 0x00, 0x7c, 0x55, 0xb8, 0x02, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0x3c, 0x25, 0x5f, 0x03, 0x40, 0x00, 0x48, 0xbe, 0x77, 0x03,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba, 0xff, 0x00, 0x00, 0x00, 0xcd,
    0x80, 0x48, 0x89, 0x04, 0x25, 0x67, 0x03, 0x40, 0x00, 0xb8, 0x03, 0x00,
    0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0xbe, 0x77, 0x03, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x14, 0x25, 0x67, 0x03, 0x40,
    0x00, 0xcd, 0x80, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x3c, 0x25,
    0x5f, 0x03, 0x40, 0x00, 0xcd, 0x80, 0xeb, 0x1b, 0xb8, 0x03, 0x00, 0x00,
    0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0xbe, 0xb3, 0x02, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xba, 0x21, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8,
    0x03, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0xbe, 0xd4,
    0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xba, 0x28, 0x00, 0x00, 0x00,
    0xcd, 0x80, 0xb8, 0x09, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0x00,
    0xcd, 0x80, 0x48, 0x89, 0x04, 0x25, 0x6f, 0x03, 0x40, 0x00, 0x48, 0x89,
    0xc7, 0x48, 0x81, 0xc7, 0x00, 0x10, 0x00, 0x00, 0xb8, 0x09, 0x00, 0x00,
    0x00, 0xcd, 0x80, 0x48, 0x83, 0xf8, 0xff, 0x74, 0x43, 0x48, 0x8b, 0x3c,
    0x25, 0x6f, 0x03, 0x40, 0x00, 0x48, 0xbe, 0xfc, 0x02, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xb9, 0x37, 0x00, 0x00, 0x00, 0xf3, 0xa4, 0xb8, 0x03,
    0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x34, 0x25,
    0x6f, 0x03, 0x40, 0x00, 0xba, 0x37, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8,
    0x09, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x3c, 0x25, 0x6f, 0x03, 0x40, 0x00,
    0xcd, 0x80, 0xeb, 0x1b, 0xb8, 0x03, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00,
    0x00, 0x00, 0x48, 0xbe, 0x33, 0x03, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xba, 0x24, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x31, 0xff, 0xcd, 0x80, 0xeb, 0xfe, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x4f, 0x6c, 0x61, 0x20, 0x64, 0x6f, 0x20, 0x52, 0x69, 0x6e, 0x67, 0x20,
    0x33, 0x21, 0x0a, 0x41, 0x63, 0x6f, 0x72, 0x64, 0x65, 0x69, 0x20, 0x61,
    0x70, 0x6f, 0x73, 0x20, 0x32, 0x73, 0x21, 0x0a
};

vfs_node_t *ramfs_init(void)
{
    /* Root directory "/" */
    ramfs_node_t *root = ramfs_mkdir_node("/");
    if (!root) return NULL;

    /* /bin/ */
    ramfs_node_t *bin = ramfs_mkdir_node("bin");
    ramfs_attach(root, bin);

    /* /fat32/ — mountpoint for FAT32 disk */
    ramfs_node_t *fat32 = ramfs_mkdir_node("fat32");
    ramfs_attach(root, fat32);

    /* /bin/hello — minimal ELF64 user process */
    ramfs_node_t *hello = ramfs_mkfile_node("hello",
                                            hello_elf,
                                            (uint32_t)sizeof(hello_elf));
    ramfs_attach(bin, hello);

    /* /etc/ */
    ramfs_node_t *etc = ramfs_mkdir_node("etc");
    ramfs_attach(root, etc);

    /* /etc/version */
    static const uint8_t version_txt[] = "ApolloOS v0.3-Alpha\n";
    ramfs_node_t *ver = ramfs_mkfile_node("version",
                                          version_txt,
                                          sizeof(version_txt) - 1);
    ramfs_attach(etc, ver);

    return &root->vfs;
}
