/*
 * DevFS — /dev device nodes
 *
 * Each node is a plain vfs_node_t with driver-specific read/write/open
 * callbacks.  vfs_open() routes the open straight to the driver, which is
 * the same pattern a future /dev/net node would use for sockets.
 */

#include <devfs.h>
#include <ramfs.h>
#include <vfs.h>
#include <ata.h>
#include <kheap.h>
#include <keyboard.h>
#include <screen.h>
#include <task.h>
#include <wait.h>
#include <stdint.h>
#include <stddef.h>

/* Raw ATA disk size in bytes (28-bit LBA sectors) */
#define HDA_SIZE_BYTES (0xFFFFFFULL * 512)

/* ---- /dev/hda — raw block device ------------------------------------------ */

static void hda_open(vfs_node_t *node) {
    (void)node;
    /* Ensure the disk was detected before first use */
    ata_detect();
}

static uint32_t hda_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)node;
    if (size == 0) return 0;

    uint32_t sector = offset / 512;
    uint32_t off_in = offset % 512;
    uint32_t bytes_read = 0;
    uint8_t sector_buf[512];

    while (bytes_read < size) {
        if (ata_read_sector(sector, sector_buf) != 0) break;

        uint32_t copy_start = off_in;
        uint32_t copy_len = 512 - copy_start;
        if (copy_len > (size - bytes_read)) copy_len = size - bytes_read;

        for (uint32_t m = 0; m < copy_len; m++)
            buf[bytes_read++] = sector_buf[copy_start + m];

        sector++;
        off_in = 0;
    }
    return bytes_read;
}

static uint32_t hda_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    (void)node;
    if (size == 0) return 0;

    uint32_t sector = offset / 512;
    uint32_t off_in = offset % 512;
    uint32_t bytes_written = 0;
    uint8_t sector_buf[512];

    while (bytes_written < size) {
        uint32_t copy_len = 512 - off_in;
        if (copy_len > (size - bytes_written)) copy_len = size - bytes_written;

        if (copy_len < 512 || off_in != 0) {
            /* Partial sector: read-modify-write through the cache */
            if (ata_read_sector(sector, sector_buf) != 0) break;
        }
        for (uint32_t m = 0; m < copy_len; m++)
            sector_buf[off_in + m] = buf[bytes_written + m];

        if (ata_write_sector(sector, sector_buf) != 0) break;
        bytes_written += copy_len;

        sector++;
        off_in = 0;
    }
    return bytes_written;
}

/* ---- /dev/tty0 — VGA text console ------------------------------------------ */

static uint32_t dev_kbd_read_line(uint32_t count, uint8_t *buf);

static void tty0_open(vfs_node_t *node) {
    (void)node;
}

static uint32_t tty0_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    (void)node; (void)offset;
    for (uint32_t i = 0; i < size; i++)
        screen_putc((char)buf[i]);
    return size;
}

static uint32_t tty0_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)node; (void)offset;
    return dev_kbd_read_line(size, buf);
}

/* ---- /dev/kbd — keyboard character device ---------------------------------- */

static void kbd_open(vfs_node_t *node) {
    (void)node;
}

static uint32_t dev_kbd_read_line(uint32_t count, uint8_t *buf) {
    uint32_t read_bytes = 0;
    while (read_bytes < count) {
        char c = keyboard_pop_char();
        if (c == 0) {
            if (read_bytes > 0) break;
            wait_event(kbd_wait, (c = keyboard_pop_char()) != 0);
        }
        buf[read_bytes++] = (uint8_t)c;
        if (c == '\n') break;
    }
    return read_bytes;
}

static uint32_t kbd_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)node; (void)offset;
    return dev_kbd_read_line(size, buf);
}

/* ---- node construction ------------------------------------------------------ */

static vfs_node_t *dev_make_node(const char *name, uint32_t flags,
                                 vfs_read_fn read, vfs_write_fn write,
                                 vfs_open_fn open) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;

    for (uint32_t i = 0; i < sizeof(vfs_node_t); i++) ((uint8_t *)node)[i] = 0;

    int n = 0;
    while (name[n] && n < VFS_MAX_NAME - 1) {
        node->name[n] = name[n];
        n++;
    }
    node->name[n] = '\0';

    node->flags = flags;
    node->read  = read;
    node->write = write;
    node->open  = open;
    node->length = (flags & VFS_BLOCKDEVICE) ? HDA_SIZE_BYTES : 0;
    return node;
}

/* ---- init ------------------------------------------------------------------ */

void devfs_init(void) {
    extern vfs_node_t *vfs_root;
    if (!vfs_root) return;

    ramfs_node_t *root = (ramfs_node_t *)vfs_root;

    ramfs_node_t *dev = ramfs_mkdir_node("dev");
    if (!dev) return;
    ramfs_attach(root, dev);

    vfs_node_t *hda  = dev_make_node("hda", VFS_BLOCKDEVICE, hda_read, hda_write, hda_open);
    vfs_node_t *tty0 = dev_make_node("tty0", VFS_CHARDEVICE, tty0_read, tty0_write, tty0_open);
    vfs_node_t *kbd  = dev_make_node("kbd", VFS_CHARDEVICE, kbd_read, NULL, kbd_open);

    if (hda)  ramfs_attach(dev, (ramfs_node_t *)hda);
    if (tty0) ramfs_attach(dev, (ramfs_node_t *)tty0);
    if (kbd)  ramfs_attach(dev, (ramfs_node_t *)kbd);

    screen_log("OK", COLOR_LIGHT_GREEN, "DevFS montado em /dev (hda, tty0, kbd).");
}