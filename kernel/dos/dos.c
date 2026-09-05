#include <dos/dos.h>
#include <dos/types.h>
#include <syscall.h>
#include <vfs.h>
#include <assign.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>

/* ------------------------------------------------------------------ */
/* Handle table — maps BPTR (1-based index) to dos_handle_t            */
/* ------------------------------------------------------------------ */

#define DOS_MAX_HANDLES  256
static dos_handle_t handle_table[DOS_MAX_HANDLES];
static int32_t last_error = 0;

/* Standard I/O */
BPTR dos_input(void)  { return 0; }
BPTR dos_output(void) { return 1; }

/* Internal: allocate a handle slot. Returns 1-based BPTR or 0 on fail. */
static BPTR alloc_handle(void) {
    for (int i = 3; i < DOS_MAX_HANDLES; i++) {
        if (handle_table[i].dh_Node == NULL) {
            handle_table[i].dh_Position = 0;
            handle_table[i].dh_Flags    = 0;
            handle_table[i].dh_ErrCode  = 0;
            return (BPTR)(i);
        }
    }
    return 0;
}

/* Internal: get handle from BPTR. Returns pointer or NULL. */
static dos_handle_t *get_handle(BPTR bptr) {
    if (bptr >= (BPTR)DOS_MAX_HANDLES) return NULL;
    dos_handle_t *h = &handle_table[bptr];
    if (h->dh_Node == NULL) return NULL;
    return h;
}

/* ------------------------------------------------------------------ */
/* dos_open — AmigaDOS file open                                       */
/* ------------------------------------------------------------------ */

BPTR dos_open(const char *name, int32_t mode) {
    if (!name || !name[0]) {
        last_error = AOS_ERR_BAD_ARGUMENT;
        return 0;
    }

    /* Resolve path through assigns */
    vfs_node_t *node = vfs_resolve(name);
    if (!node) {
        last_error = AOS_ERR_NOT_FOUND;
        return 0;
    }

    /* MODE_NEWFILE: if file doesn't exist, we'd create it.
     * For now, just open existing. */
    (void)mode;

    /* Allocate handle */
    BPTR bptr = alloc_handle();
    if (!bptr) {
        last_error = AOS_ERR_NO_MEMORY;
        return 0;
    }

    dos_handle_t *h = &handle_table[bptr];
    h->dh_Node     = node;
    h->dh_Position = 0;
    h->dh_Flags    = (uint32_t)mode;
    h->dh_ErrCode  = 0;

    /* Call VFS open */
    vfs_open(node);

    serial_print("DOS: Open \"");
    serial_print(name);
    serial_print("\" → BPTR=");
    char buf[16];
    itoa(bptr, buf, 10);
    serial_print(buf);
    serial_print("\n");

    return bptr;
}

/* ------------------------------------------------------------------ */
/* dos_close — AmigaDOS file close                                      */
/* ------------------------------------------------------------------ */

void dos_close(BPTR handle) {
    dos_handle_t *h = get_handle(handle);
    if (!h) return;

    serial_print("DOS: Close BPTR=");
    char buf[16];
    itoa(handle, buf, 10);
    serial_print(buf);
    serial_print("\n");

    vfs_close(h->dh_Node);
    h->dh_Node = NULL;
}

/* ------------------------------------------------------------------ */
/* dos_read — AmigaDOS file read                                        */
/* ------------------------------------------------------------------ */

int32_t dos_read(BPTR handle, void *buffer, int32_t length) {
    dos_handle_t *h = get_handle(handle);
    if (!h || !buffer || length <= 0) {
        last_error = AOS_ERR_BAD_ARGUMENT;
        return -1;
    }

    uint32_t got = vfs_read(h->dh_Node, (uint32_t)h->dh_Position,
                             (uint32_t)length, (uint8_t *)buffer);
    h->dh_Position += got;

    return (int32_t)got;
}

/* ------------------------------------------------------------------ */
/* dos_write — AmigaDOS file write                                      */
/* ------------------------------------------------------------------ */

int32_t dos_write(BPTR handle, const void *buffer, int32_t length) {
    dos_handle_t *h = get_handle(handle);
    if (!h || !buffer || length <= 0) {
        last_error = AOS_ERR_BAD_ARGUMENT;
        return -1;
    }

    uint32_t written = vfs_write(h->dh_Node, (uint32_t)h->dh_Position,
                                  (uint32_t)length, (const uint8_t *)buffer);
    h->dh_Position += written;

    return (int32_t)written;
}

/* ------------------------------------------------------------------ */
/* dos_seek — AmigaDOS file seek                                        */
/* ------------------------------------------------------------------ */

int32_t dos_seek(BPTR handle, int32_t position, int32_t offset_type) {
    dos_handle_t *h = get_handle(handle);
    if (!h) {
        last_error = AOS_ERR_BAD_ARGUMENT;
        return -1;
    }

    int64_t new_pos = (int64_t)h->dh_Position;

    switch (offset_type) {
        case OFFSET_BEGINNING:
            new_pos = (int64_t)position;
            break;
        case OFFSET_CURRENT:
            new_pos += (int64_t)position;
            break;
        case OFFSET_END:
            new_pos = (int64_t)h->dh_Node->length + (int64_t)position;
            break;
        default:
            last_error = AOS_ERR_BAD_ARGUMENT;
            return -1;
    }

    if (new_pos < 0) new_pos = 0;
    h->dh_Position = (uint32_t)new_pos;

    return (int32_t)new_pos;
}

/* ------------------------------------------------------------------ */
/* dos_flush — AmigaDOS file flush                                     */
/* ------------------------------------------------------------------ */

int32_t dos_flush(BPTR handle) {
    (void)handle;
    /* For now, no-op (write-back cache handles this) */
    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_examine — fill FileInfoBlock from a VFS node                    */
/* ------------------------------------------------------------------ */

static void node_to_fib(vfs_node_t *node, file_info_block_t *fib) {
    memset(fib, 0, sizeof(file_info_block_t));

    fib->fib_DiskKey = (int32_t)node->inode;

    if (node->flags & VFS_DIRECTORY) {
        fib->fib_DirEntryType = 1;  /* >0 = directory */
    } else {
        fib->fib_DirEntryType = -1; /* <0 = file */
    }

    /* Copy filename */
    int i;
    for (i = 0; i < DOS_FILENAMESIZE - 1 && node->name[i]; i++)
        fib->fib_FileName[i] = node->name[i];
    fib->fib_FileName[i] = 0;

    fib->fib_Protection = AOS_FIBF_READ | AOS_FIBF_WRITE | AOS_FIBF_EXECUTE;
    fib->fib_Size       = (int32_t)node->length;
    fib->fib_NumBlocks  = (node->length + 4095) / 4096;
}

int32_t dos_examine(BPTR lock, file_info_block_t *fib) {
    if (!fib) {
        last_error = AOS_ERR_BAD_ARGUMENT;
        return -1;
    }

    dos_handle_t *h = get_handle(lock);
    if (!h || !h->dh_Node) {
        last_error = AOS_ERR_NOT_FOUND;
        return -1;
    }

    node_to_fib(h->dh_Node, fib);
    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_ex_next — iterate directory entries                             */
/* ------------------------------------------------------------------ */

/* State for directory iteration, stored in handle's dh_Position */
int32_t dos_ex_next(BPTR lock, file_info_block_t *fib) {
    dos_handle_t *h = get_handle(lock);
    if (!h || !h->dh_Node) {
        last_error = AOS_ERR_NOT_FOUND;
        return -1;
    }

    if (!(h->dh_Node->flags & VFS_DIRECTORY)) {
        last_error = AOS_ERR_IS_DIRECTORY;
        return -1;
    }

    uint32_t idx = h->dh_Position;
    vfs_dirent_t *entry = vfs_readdir(h->dh_Node, idx);
    if (!entry) {
        /* No more entries */
        last_error = AOS_ERR_NOT_FOUND;
        return -1;
    }

    h->dh_Position++;

    /* Find the actual node for this entry */
    vfs_node_t *child = vfs_finddir(h->dh_Node, entry->name);
    if (child) {
        node_to_fib(child, fib);
    } else {
        /* Entry exists but node not found — just fill name */
        memset(fib, 0, sizeof(file_info_block_t));
        int i;
        for (i = 0; i < DOS_FILENAMESIZE - 1 && entry->name[i]; i++)
            fib->fib_FileName[i] = entry->name[i];
        fib->fib_FileName[i] = 0;
        fib->fib_DirEntryType = -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_lock / dos_un_lock — directory lock (simplified)                */
/* ------------------------------------------------------------------ */

BPTR dos_lock(const char *name, int32_t lock_type) {
    (void)lock_type;
    if (!name) return 0;

    vfs_node_t *node = vfs_resolve(name);
    if (!node) {
        last_error = AOS_ERR_NOT_FOUND;
        return 0;
    }

    /* Allocate a handle for the lock */
    BPTR bptr = alloc_handle();
    if (!bptr) {
        last_error = AOS_ERR_NO_MEMORY;
        return 0;
    }

    dos_handle_t *h = &handle_table[bptr];
    h->dh_Node     = node;
    h->dh_Position = 0;  /* for ExNext iteration */
    h->dh_Flags    = (uint32_t)lock_type;
    h->dh_ErrCode  = 0;

    return bptr;
}

void dos_un_lock(BPTR lock) {
    dos_close(lock);
}

/* ------------------------------------------------------------------ */
/* dos_create_dir / dos_delete_file / dos_rename                       */
/* ------------------------------------------------------------------ */

int32_t dos_create_dir(const char *name) {
    if (!name) return -1;

    /* Find parent and leaf */
    char parent_path[ASSIGN_MAX_PATH] = {0};
    const char *leaf = name;
    const char *last_sep = NULL;

    for (int i = 0; name[i]; i++) {
        if (name[i] == '/' || name[i] == ':') last_sep = name + i;
    }

    if (last_sep) {
        int plen = (int)(last_sep - name + 1);
        if (plen >= ASSIGN_MAX_PATH) plen = ASSIGN_MAX_PATH - 1;
        for (int i = 0; i < plen; i++) parent_path[i] = name[i];
        parent_path[plen] = 0;
        if (parent_path[plen - 1] == '/') parent_path[plen - 1] = 0;
        if (parent_path[0] == 0) strcpy(parent_path, "Work:");
        leaf = last_sep + 1;
    } else {
        strcpy(parent_path, "Work:");
        leaf = name;
    }

    vfs_node_t *parent = vfs_resolve(parent_path);
    if (!parent) {
        last_error = AOS_ERR_NOT_FOUND;
        return -1;
    }

    int rc = vfs_mkdir(parent, leaf, 0);
    if (rc != 0) last_error = AOS_ERR_NOT_FOUND;
    return rc;
}

int32_t dos_delete_file(const char *name) {
    if (!name) return -1;

    /* Find parent and leaf */
    char parent_path[ASSIGN_MAX_PATH] = {0};
    const char *leaf = name;
    const char *last_sep = NULL;

    for (int i = 0; name[i]; i++) {
        if (name[i] == '/' || name[i] == ':') last_sep = name + i;
    }

    if (last_sep) {
        int plen = (int)(last_sep - name + 1);
        if (plen >= ASSIGN_MAX_PATH) plen = ASSIGN_MAX_PATH - 1;
        for (int i = 0; i < plen; i++) parent_path[i] = name[i];
        parent_path[plen] = 0;
        if (parent_path[plen - 1] == '/') parent_path[plen - 1] = 0;
        if (parent_path[0] == 0) strcpy(parent_path, "Work:");
        leaf = last_sep + 1;
    } else {
        strcpy(parent_path, "Work:");
        leaf = name;
    }

    vfs_node_t *parent = vfs_resolve(parent_path);
    if (!parent) {
        last_error = AOS_ERR_NOT_FOUND;
        return -1;
    }

    int rc = vfs_unlink(parent, leaf);
    if (rc != 0) rc = vfs_rmdir(parent, leaf);
    if (rc != 0) last_error = AOS_ERR_NOT_FOUND;
    return rc;
}

int32_t dos_rename(const char *old_name, const char *new_name) {
    if (!old_name || !new_name) return -1;

    /* Resolve old parent + leaf */
    char old_parent_path[ASSIGN_MAX_PATH] = {0};
    const char *old_leaf = old_name;
    const char *old_sep = NULL;
    for (int i = 0; old_name[i]; i++)
        if (old_name[i] == '/' || old_name[i] == ':') old_sep = old_name + i;

    if (old_sep) {
        int plen = (int)(old_sep - old_name + 1);
        if (plen >= ASSIGN_MAX_PATH) plen = ASSIGN_MAX_PATH - 1;
        for (int i = 0; i < plen; i++) old_parent_path[i] = old_name[i];
        old_parent_path[plen] = 0;
        if (old_parent_path[plen - 1] == '/') old_parent_path[plen - 1] = 0;
        if (old_parent_path[0] == 0) strcpy(old_parent_path, "Work:");
        old_leaf = old_sep + 1;
    } else {
        strcpy(old_parent_path, "Work:");
        old_leaf = old_name;
    }

    /* Resolve new parent + leaf */
    char new_parent_path[ASSIGN_MAX_PATH] = {0};
    const char *new_leaf = new_name;
    const char *new_sep = NULL;
    for (int i = 0; new_name[i]; i++)
        if (new_name[i] == '/' || new_name[i] == ':') new_sep = new_name + i;

    if (new_sep) {
        int plen = (int)(new_sep - new_name + 1);
        if (plen >= ASSIGN_MAX_PATH) plen = ASSIGN_MAX_PATH - 1;
        for (int i = 0; i < plen; i++) new_parent_path[i] = new_name[i];
        new_parent_path[plen] = 0;
        if (new_parent_path[plen - 1] == '/') new_parent_path[plen - 1] = 0;
        if (new_parent_path[0] == 0) strcpy(new_parent_path, "Work:");
        new_leaf = new_sep + 1;
    } else {
        strcpy(new_parent_path, "Work:");
        new_leaf = new_name;
    }

    vfs_node_t *old_parent = vfs_resolve(old_parent_path);
    vfs_node_t *new_parent = vfs_resolve(new_parent_path);
    if (!old_parent || !new_parent) {
        last_error = AOS_ERR_NOT_FOUND;
        return -1;
    }

    int rc = vfs_rename(old_parent, old_leaf, new_parent, new_leaf);
    if (rc != 0) last_error = AOS_ERR_NOT_FOUND;
    return rc;
}

/* ------------------------------------------------------------------ */
/* dos_current_dir / dos_name_from_lock                                */
/* ------------------------------------------------------------------ */

int32_t dos_current_dir(const char *name) {
    if (!name) return -1;
    /* Delegate to assign system (Work: is the CWD) */
    return assign_set("Work", name);
}

int32_t dos_name_from_lock(BPTR lock, char *name, int32_t len) {
    dos_handle_t *h = get_handle(lock);
    if (!h || !h->dh_Node || !name || len <= 0) {
        last_error = AOS_ERR_BAD_ARGUMENT;
        return -1;
    }

    /* For now, return the node's name */
    int i;
    for (i = 0; i < len - 1 && h->dh_Node->name[i]; i++)
        name[i] = h->dh_Node->name[i];
    name[i] = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Error handling                                                       */
/* ------------------------------------------------------------------ */

int32_t dos_io_err(void) {
    return last_error;
}

void dos_set_io_err(int32_t err) {
    last_error = err;
}

/* ------------------------------------------------------------------ */
/* Init — zero handle table                                            */
/* ------------------------------------------------------------------ */

void dos_init(void) {
    memset(handle_table, 0, sizeof(handle_table));
    last_error = 0;

    /* stdin/stdout/stderr are reserved (handled by TTY) */
    handle_table[0].dh_Node = (vfs_node_t *)(uintptr_t)0xFFFFFFFF; /* sentinel */
    handle_table[1].dh_Node = (vfs_node_t *)(uintptr_t)0xFFFFFFFF;
    handle_table[2].dh_Node = (vfs_node_t *)(uintptr_t)0xFFFFFFFF;

    serial_print("DOS: handle table initialized\n");
}
