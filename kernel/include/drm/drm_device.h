#ifndef _DRM_DEVICE_H_
#define _DRM_DEVICE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <spinlock.h>

/* IRQ return type — shared with kernel IRQ handlers */
#ifndef _IRQRETURN_T_DEFINED
#define _IRQRETURN_T_DEFINED
typedef enum {
    IRQ_NONE    = 0,
    IRQ_HANDLED = 1,
} irqreturn_t;
#endif

struct drm_driver;
struct drm_file;
struct drm_minor;

#define DRM_MAX_DEVICES      32
#define DRM_MAX_MINOR        16
#define DRM_MAX_OPEN_FILES   256
#define DRM_IOCTL_TABLE_SIZE 64

/* DRM ioctl return codes */
#define DRM_NODEVICE    0x01000000
#define DRM_AUTH        0x02000000
#define DRM_MASTER      0x04000000

/* Forward declarations */
typedef struct drm_device drm_device_t;
typedef struct drm_file   drm_file_t;
typedef struct drm_minor  drm_minor_t;

/* DRM ioctl handler function type */
typedef int (*drm_ioctl_t)(drm_device_t *dev, unsigned int cmd,
                           unsigned long arg);

/* DRM ioctl table entry */
struct drm_ioctl_desc {
    drm_ioctl_t   func;
    const char    *name;
    uint32_t      flags;
};

/* Per-open file state */
struct drm_file {
    bool            authenticated;
    bool            is_master;
    uint32_t        pid;
    void           *driver_priv;
    drm_device_t   *dev;
    int             minor_id;
};

/* Minor device (maps to /dev/dri/cardN) */
struct drm_minor {
    int              index;
    int              type;          /* DRM_MINORTYPE_PRIMARY etc. */
    drm_device_t    *dev;
    struct drm_file *open_files[DRM_MAX_OPEN_FILES];
    int              num_open_files;
    spinlock_t       file_lock;
};

/* Main DRM device structure */
struct drm_device {
    const struct drm_driver *driver;
    void                    *dev_private;

    /* MMIO register space */
    void                    *registers;
    size_t                   register_size;

    /* PCI location */
    uint8_t                  pci_bus;
    uint8_t                  pci_dev;
    uint8_t                  pci_func;

    /* Minor device info */
    drm_minor_t             *minor;
    int                      minor_id;

    /* Device state */
    bool                     registered;
    bool                     enabled;
    int                      open_count;
    spinlock_t               struct_lock;

    /* Memory management */
    struct {
        void    *vram;
        size_t   vram_size;
        void    *gart;
        size_t   gart_size;
    } memory;

    /* Command submission */
    struct {
        void        *ring_buffer;
        size_t       ring_size;
        uint32_t    *write_ptr;
        uint32_t    *read_ptr;
    } ring;

    /* Fence management */
    struct {
        uint32_t     last_seqno;
        uint32_t     waited_seqno;
    } fence;

    /* Display / KMS */
    struct {
        uint32_t     crtc_id;
        uint32_t     encoder_id;
        uint32_t     connector_id;
        struct {
            uint32_t width;
            uint32_t height;
            uint32_t pitch;
            uint32_t *buffer;
        } mode;
    } kms;
};

#endif /* _DRM_DEVICE_H_ */
