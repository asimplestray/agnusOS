/*
 * DRM Core — Device registration, /dev/dri/card0, ioctl dispatch
 *
 * Replaces the old drm_core.c stubs with a real implementation.
 * drm_init() runs at boot and creates /dev/dri/card0 via devfs.
 */

#include <drm/drm_device.h>
#include <drm/drm_driver.h>
#include <devfs.h>
#include <ramfs.h>
#include <vfs.h>
#include <pci.h>
#include <workqueue.h>
#include <spinlock.h>
#include <serial.h>
#include <screen.h>
#include <string.h>
#include <kheap.h>
#include <syscall.h>

/* Errno codes not in shared headers */
#ifndef ENODEV
#define ENODEV   19
#endif
#ifndef ENOTTY
#define ENOTTY   25
#endif
#ifndef EINVAL
#define EINVAL   22
#endif

/* PCI subsystem IDs (may not be in pci.h yet) */
#ifndef PCI_ANY_ID
#define PCI_ANY_ID (~0)
#endif
#ifndef PCI_CONFIG_SUBSYSTEM_VENDOR_ID
#define PCI_CONFIG_SUBSYSTEM_VENDOR_ID 0x2C
#endif
#ifndef PCI_CONFIG_SUBSYSTEM_ID
#define PCI_CONFIG_SUBSYSTEM_ID        0x2E
#endif

/* ---- Global state ------------------------------------------------------- */

static drm_device_t *drm_devices[DRM_MAX_DEVICES];
static int            drm_device_count;
static spinlock_t     drm_devices_lock = SPINLOCK_INIT;

static drm_minor_t    drm_minors[DRM_MAX_MINOR];
static int            drm_minor_count;
static spinlock_t     drm_minors_lock = SPINLOCK_INIT;

/* ---- DRM ioctl core handlers ------------------------------------------- */

/* DRM_IOCTL_VERSION — return driver name/version */
static int drm_core_ioctl_version(drm_device_t *dev, unsigned int cmd,
                                  unsigned long arg)
{
    (void)cmd; (void)arg;
    if (!dev || !dev->driver || !dev->driver->version)
        return -EINVAL;

    const struct drm_driver_version *ver = dev->driver->version;
    serial_print("DRM: ioctl VERSION -> ");
    serial_print(ver->name);
    serial_print("\n");
    /* TODO: copy version info to userspace arg when copy_to_user exists */
    return 0;
}

/* DRM_IOCTL_GET_MAGIC — return auth magic for device */
static int drm_core_ioctl_get_magic(drm_device_t *dev, unsigned int cmd,
                                    unsigned long arg)
{
    (void)cmd; (void)arg; (void)dev;
    /* TODO: generate and return magic number */
    return 0;
}

/* DRM_IOCTL_AUTH_MAGIC — authenticate a magic number */
static int drm_core_ioctl_auth_magic(drm_device_t *dev, unsigned int cmd,
                                     unsigned long arg)
{
    (void)cmd; (void)arg; (void)dev;
    /* TODO: look up magic and mark file as authenticated */
    return 0;
}

/* DRM_IOCTL_GET_UNIQUE — return device unique name */
static int drm_core_ioctl_get_unique(drm_device_t *dev, unsigned int cmd,
                                     unsigned long arg)
{
    (void)cmd; (void)arg;
    if (!dev || !dev->driver || !dev->driver->version)
        return -EINVAL;

    serial_print("DRM: ioctl GET_UNIQUE -> ");
    serial_print(dev->driver->version->name);
    serial_print("\n");
    return 0;
}

/* DRM_IOCTL_GET_CLIENT — enumerate open clients */
static int drm_core_ioctl_get_client(drm_device_t *dev, unsigned int cmd,
                                     unsigned long arg)
{
    (void)cmd; (void)arg; (void)dev;
    /* TODO: enumerate open drm_file clients */
    return 0;
}

/* DRM_IOCTL_GET_STATS — return device stats */
static int drm_core_ioctl_get_stats(drm_device_t *dev, unsigned int cmd,
                                    unsigned long arg)
{
    (void)cmd; (void)arg; (void)dev;
    /* TODO: return DRM stats (bytes/cards/etc) */
    return 0;
}

/* DRM core ioctl dispatch table */
static const struct drm_ioctl_desc drm_core_ioctls[] = {
    [DRM_IOCTL_VERSION]    = { .func = drm_core_ioctl_version,
                               .name = "VERSION",    .flags = 0 },
    [DRM_IOCTL_GET_MAGIC]  = { .func = drm_core_ioctl_get_magic,
                               .name = "GET_MAGIC",  .flags = DRM_AUTH },
    [DRM_IOCTL_AUTH_MAGIC] = { .func = drm_core_ioctl_auth_magic,
                               .name = "AUTH_MAGIC", .flags = DRM_MASTER },
    [DRM_IOCTL_GET_UNIQUE] = { .func = drm_core_ioctl_get_unique,
                               .name = "GET_UNIQUE", .flags = 0 },
    [DRM_IOCTL_GET_CLIENT] = { .func = drm_core_ioctl_get_client,
                               .name = "GET_CLIENT", .flags = 0 },
    [DRM_IOCTL_GET_STATS]  = { .func = drm_core_ioctl_get_stats,
                               .name = "GET_STATS",  .flags = 0 },
};
#define DRM_CORE_IOCTL_COUNT (sizeof(drm_core_ioctls) / sizeof(drm_core_ioctls[0]))

/* ---- ioctl dispatch ---------------------------------------------------- */

int drm_ioctl(drm_device_t *dev, unsigned int cmd, unsigned long arg)
{
    if (!dev)
        return -EINVAL;

    /* Core ioctls (below DRM_COMMAND_BASE) */
    if (cmd < DRM_COMMAND_BASE) {
        if (cmd < DRM_CORE_IOCTL_COUNT && drm_core_ioctls[cmd].func) {
            return drm_core_ioctls[cmd].func(dev, cmd, arg);
        }
        return -ENOTTY;
    }

    /* Driver-specific ioctls */
    unsigned int nr = cmd - DRM_COMMAND_BASE;
    if (dev->driver && dev->driver->ioctls && nr < dev->driver->num_ioctls) {
        const struct drm_ioctl_desc *desc = &dev->driver->ioctls[nr];
        if (desc->func)
            return desc->func(dev, cmd, arg);
    }

    return -ENOTTY;
}

/* ---- Minor device management ------------------------------------------- */

static drm_minor_t *drm_alloc_minor(void)
{
    spin_lock(&drm_minors_lock);

    for (int i = 0; i < DRM_MAX_MINOR; i++) {
        if (drm_minors[i].dev == NULL) {
            drm_minor_t *minor = &drm_minors[i];
            memset(minor, 0, sizeof(*minor));
            minor->index = i;
            minor->type = 0; /* PRIMARY */
            spinlock_init(&minor->file_lock);
            spin_unlock(&drm_minors_lock);
            return minor;
        }
    }

    spin_unlock(&drm_minors_lock);
    return NULL;
}

static void drm_free_minor(drm_minor_t *minor)
{
    if (!minor) return;

    spin_lock(&drm_minors_lock);
    spin_lock(&minor->file_lock);
    minor->dev = NULL;
    minor->num_open_files = 0;
    spin_unlock(&minor->file_lock);
    spin_unlock(&drm_minors_lock);
}

/* ---- File operations for /dev/dri/cardN -------------------------------- */

/* Forward declaration */
static uint32_t drm_dev_read(vfs_node_t *node, uint32_t offset,
                             uint32_t size, uint8_t *buf);
static uint32_t drm_dev_write(vfs_node_t *node, uint32_t offset,
                              uint32_t size, const uint8_t *buf);
static void drm_dev_open(vfs_node_t *node);

static drm_device_t *drm_node_to_device(vfs_node_t *node)
{
    if (!node) return NULL;

    /* The node->ptr field stores the drm_minor_t pointer */
    drm_minor_t *minor = (drm_minor_t *)node->ptr;
    if (minor)
        return minor->dev;
    return NULL;
}

static void drm_dev_open(vfs_node_t *node)
{
    drm_device_t *dev = drm_node_to_device(node);
    if (!dev) return;

    spin_lock(&dev->struct_lock);
    dev->open_count++;

    /* Create a new drm_file for this open */
    drm_minor_t *minor = dev->minor;
    if (minor) {
        spin_lock(&minor->file_lock);
        if (minor->num_open_files < DRM_MAX_OPEN_FILES) {
            drm_file_t *file = (drm_file_t *)kmalloc(sizeof(drm_file_t));
            if (file) {
                memset(file, 0, sizeof(*file));
                file->dev = dev;
                file->minor_id = minor->index;
                file->authenticated = false;
                file->is_master = (minor->num_open_files == 0);
                minor->open_files[minor->num_open_files++] = file;
            }
        }
        spin_unlock(&minor->file_lock);
    }

    spin_unlock(&dev->struct_lock);

    if (dev->driver && dev->driver->open)
        dev->driver->open(dev, NULL);

    serial_print("DRM: device opened\n");
}

/* Read from DRM device — dispatches to ioctl-style interface */
static uint32_t drm_dev_read(vfs_node_t *node, uint32_t offset,
                             uint32_t size, uint8_t *buf)
{
    (void)offset; (void)size; (void)buf;
    drm_device_t *dev = drm_node_to_device(node);
    if (!dev) return 0;

    serial_print("DRM: read on card device\n");
    return 0;
}

/* Write to DRM device */
static uint32_t drm_dev_write(vfs_node_t *node, uint32_t offset,
                              uint32_t size, const uint8_t *buf)
{
    (void)offset; (void)size; (void)buf;
    drm_device_t *dev = drm_node_to_device(node);
    if (!dev) return 0;

    serial_print("DRM: write on card device\n");
    return 0;
}

/* ---- Device management -------------------------------------------------- */

static int drm_find_free_slot(void)
{
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_devices[i] == NULL)
            return i;
    }
    return -1;
}

drm_device_t *drm_device_create_from_pci(uint8_t bus, uint8_t dev,
                                         uint8_t func,
                                         const struct drm_driver *driver)
{
    drm_device_t *drm_dev;
    int slot;

    slot = drm_find_free_slot();
    if (slot < 0) {
        serial_print("DRM: no free device slots\n");
        return NULL;
    }

    drm_dev = (drm_device_t *)kmalloc(sizeof(drm_device_t));
    if (!drm_dev) {
        serial_print("DRM: failed to allocate device\n");
        return NULL;
    }
    memset(drm_dev, 0, sizeof(*drm_dev));

    spinlock_init(&drm_dev->struct_lock);

    drm_dev->pci_bus = bus;
    drm_dev->pci_dev = dev;
    drm_dev->pci_func = func;
    drm_dev->driver = driver;
    drm_dev->registered = false;
    drm_dev->minor_id = slot;

    /* Read PCI IDs */
    uint16_t vendor = pci_read_word(bus, dev, func, PCI_CONFIG_VENDOR_ID);
    uint16_t device = pci_read_word(bus, dev, func, PCI_CONFIG_DEVICE_ID);

    serial_print("DRM: found device ");
    /* Print vendor:device in hex via serial */
    {
        char buf[32];
        int n = 0;
        const char *hex = "0123456789abcdef";
        buf[n++] = hex[(vendor >> 12) & 0xF];
        buf[n++] = hex[(vendor >> 8)  & 0xF];
        buf[n++] = hex[(vendor >> 4)  & 0xF];
        buf[n++] = hex[vendor & 0xF];
        buf[n++] = ':';
        buf[n++] = hex[(device >> 12) & 0xF];
        buf[n++] = hex[(device >> 8)  & 0xF];
        buf[n++] = hex[(device >> 4)  & 0xF];
        buf[n++] = hex[device & 0xF];
        buf[n] = '\0';
        serial_print(buf);
    }
    serial_print("\n");

    /* Map BAR0 (MMIO registers) */
    uint32_t bar0 = pci_read_dword(bus, dev, func, PCI_CONFIG_BAR0);
    if (!(bar0 & 0x1)) { /* Memory space */
        uint64_t bar_addr = (uint64_t)(bar0 & ~0xFULL);
        drm_dev->registers = (void *)bar_addr;
        drm_dev->register_size = 0x10000; /* 64KB default */
        pci_map_bar(bus, dev, func, 0, bar_addr);
    }

    /* Register in global list */
    spin_lock(&drm_devices_lock);
    drm_devices[slot] = drm_dev;
    drm_device_count++;
    spin_unlock(&drm_devices_lock);

    return drm_dev;
}

int drm_register_driver(const struct drm_driver *driver,
                        const struct drm_pci_id *pci_ids)
{
    (void)pci_ids;
    if (!driver || !driver->version)
        return -EINVAL;

    serial_print("DRM: registering driver ");
    serial_print(driver->version->name);
    serial_print("\n");
    return 0;
}

void drm_unregister_driver(const struct drm_driver *driver)
{
    if (!driver) return;
    serial_print("DRM: unregistering driver ");
    if (driver->version)
        serial_print(driver->version->name);
    serial_print("\n");
}

drm_device_t *drm_get_device(uint16_t vendor_id, uint16_t device_id)
{
    spin_lock(&drm_devices_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_devices[i] && drm_devices[i]->registered) {
            uint8_t bus = drm_devices[i]->pci_bus;
            uint8_t dev_num = drm_devices[i]->pci_dev;
            uint8_t func = drm_devices[i]->pci_func;

            uint16_t v = pci_read_word(bus, dev_num, func,
                                       PCI_CONFIG_VENDOR_ID);
            uint16_t d = pci_read_word(bus, dev_num, func,
                                       PCI_CONFIG_DEVICE_ID);

            if (v == vendor_id && d == device_id) {
                spin_unlock(&drm_devices_lock);
                return drm_devices[i];
            }
        }
    }
    spin_unlock(&drm_devices_lock);
    return NULL;
}

void drm_put_device(drm_device_t *dev)
{
    if (!dev) return;
    /* TODO: reference counting */
}

/* ---- PCI probe/remove --------------------------------------------------- */

int drm_pci_device_probe(uint8_t bus, uint8_t dev, uint8_t func,
                         const struct drm_driver *driver,
                         const struct drm_pci_id *pci_ids)
{
    drm_device_t *drm_dev;

    uint16_t vendor = pci_read_word(bus, dev, func, PCI_CONFIG_VENDOR_ID);
    uint16_t device = pci_read_word(bus, dev, func, PCI_CONFIG_DEVICE_ID);

    for (int i = 0; pci_ids[i].vendor_id != 0 || pci_ids[i].device_id != 0; i++) {
        if (pci_ids[i].vendor_id != PCI_ANY_ID &&
            pci_ids[i].vendor_id != vendor)
            continue;
        if (pci_ids[i].device_id != PCI_ANY_ID &&
            pci_ids[i].device_id != device)
            continue;
        if (pci_ids[i].subvendor_id != PCI_ANY_ID &&
            pci_ids[i].subvendor_id !=
                pci_read_word(bus, dev, func, PCI_CONFIG_SUBSYSTEM_VENDOR_ID))
            continue;
        if (pci_ids[i].subdevice_id != PCI_ANY_ID &&
            pci_ids[i].subdevice_id !=
                pci_read_word(bus, dev, func, PCI_CONFIG_SUBSYSTEM_ID))
            continue;

        /* Match found — create device */
        drm_dev = drm_device_create_from_pci(bus, dev, func, driver);
        if (!drm_dev)
            return -ENOMEM;

        /* Associate minor device */
        drm_minor_t *minor = drm_alloc_minor();
        if (minor) {
            minor->dev = drm_dev;
            drm_dev->minor = minor;
        }

        /* Call driver load */
        int ret = driver->load(drm_dev);
        if (ret) {
            serial_print("DRM: driver load failed\n");
            drm_free_minor(drm_dev->minor);
            drm_dev->minor = NULL;
            kfree(drm_dev);
            return ret;
        }

        drm_dev->registered = true;
        serial_print("DRM: registered device\n");
        return 0;
    }

    return -ENODEV;
}

void drm_pci_device_remove(uint8_t bus, uint8_t dev, uint8_t func,
                           const struct drm_driver *driver)
{
    (void)bus; (void)dev; (void)func;

    spin_lock(&drm_devices_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_devices[i] && drm_devices[i]->driver == driver) {
            drm_device_t *drm_dev = drm_devices[i];

            if (drm_dev->registered && drm_dev->driver->unload)
                drm_dev->driver->unload(drm_dev);

            if (drm_dev->minor) {
                drm_free_minor(drm_dev->minor);
                drm_dev->minor = NULL;
            }

            drm_devices[i] = NULL;
            drm_device_count--;
            kfree(drm_dev);
            serial_print("DRM: device removed\n");
            break;
        }
    }
    spin_unlock(&drm_devices_lock);
}

/* ---- /dev/dri node creation for devfs ---------------------------------- */

/* VFS node for /dev/dri directory (read readdir returns card0, etc.) */
static vfs_node_t *dri_dir_node;

/* Create a VFS node for a DRM card device */
static vfs_node_t *drm_create_card_node(drm_minor_t *minor, int card_num)
{
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));

    /* Set name: card0, card1, etc. */
    node->name[0] = 'c';
    node->name[1] = 'a';
    node->name[2] = 'r';
    node->name[3] = 'd';
    node->name[4] = '0' + card_num;
    node->name[5] = '\0';

    node->flags = VFS_CHARDEVICE;
    node->read  = drm_dev_read;
    node->write = drm_dev_write;
    node->open  = drm_dev_open;
    node->close = NULL;
    node->length = 0;

    /* Store minor pointer in node->ptr for lookup */
    node->ptr = (vfs_node_t *)minor;

    return node;
}

/* ---- DRM subsystem init ------------------------------------------------- */

void drm_init(void)
{
    serial_print("DRM: initializing DRM subsystem\n");

    /* Clear global state */
    memset(drm_devices, 0, sizeof(drm_devices));
    drm_device_count = 0;
    memset(drm_minors, 0, sizeof(drm_minors));
    drm_minor_count = 0;

    /* Create /dev/dri directory and /dev/dri/card0 node */
    extern vfs_node_t *vfs_root;
    if (!vfs_root) {
        serial_print("DRM: VFS root not available, deferring devfs setup\n");
        return;
    }

    ramfs_node_t *root = (ramfs_node_t *)vfs_root;

    /* Find existing /dev directory */
    ramfs_node_t *dev_dir = NULL;
    for (uint32_t i = 0; i < root->num_children; i++) {
        if (root->children[i] &&
            strcmp(root->children[i]->vfs.name, "dev") == 0) {
            dev_dir = root->children[i];
            break;
        }
    }

    if (!dev_dir) {
        serial_print("DRM: /dev not found\n");
        return;
    }

    /* Create /dev/dri directory */
    ramfs_node_t *dri_dir = ramfs_mkdir_node("dri");
    if (!dri_dir) {
        serial_print("DRM: failed to create /dev/dri\n");
        return;
    }
    ramfs_attach(dev_dir, dri_dir);
    dri_dir_node = (vfs_node_t *)dri_dir;

    /* Pre-allocate a minor for card0 */
    drm_minor_t *card0_minor = drm_alloc_minor();
    if (card0_minor) {
        drm_minor_count = 1;

        /* Create /dev/dri/card0 VFS node */
        vfs_node_t *card0 = drm_create_card_node(card0_minor, 0);
        if (card0) {
            ramfs_attach(dri_dir, (ramfs_node_t *)card0);
            serial_print("DRM: created /dev/dri/card0\n");
        } else {
            serial_print("DRM: failed to create card0 node\n");
        }
    }

    screen_log("OK", COLOR_LIGHT_GREEN, "DRM core initialized (/dev/dri/card0).");
    serial_print("DRM: init complete\n");
}
