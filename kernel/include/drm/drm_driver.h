#ifndef _DRM_DRIVER_H_
#define _DRM_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>
#include "drm_device.h"

struct drm_device;

/* DRM driver version */
struct drm_driver_version {
    int         major;
    int         minor;
    int         patch;
    const char *name;
    const char *date;
    const char *desc;
};

/* DRM driver callbacks */
struct drm_driver {
    const struct drm_driver_version *version;

    /* Device lifecycle */
    int  (*load)(struct drm_device *dev);
    int  (*unload)(struct drm_device *dev);
    int  (*open)(struct drm_device *dev, void *file_private);
    int  (*release)(struct drm_device *dev, void *file_private);

    /* Memory management */
    int  (*gem_create_object)(struct drm_device *dev, size_t size, void **obj);
    void (*gem_free_object)(struct drm_device *dev, void *obj);
    int  (*gem_pin_object)(struct drm_device *dev, void *obj, uint64_t *alignment);
    void (*gem_unpin_object)(struct drm_device *dev, void *obj);

    /* Command submission */
    int  (*submit_command)(struct drm_device *dev, void *cmd, size_t cmd_size);
    int  (*wait_for_fence)(struct drm_device *dev, uint32_t seqno, uint64_t timeout_ns);
    void (*signal_fence)(struct drm_device *dev, uint32_t seqno);

    /* Display / KMS */
    int  (*mode_set)(struct drm_device *dev, uint32_t width, uint32_t height,
                     uint32_t refresh_rate);
    int  (*mode_get)(struct drm_device *dev, uint32_t *width, uint32_t *height,
                     uint32_t *refresh_rate);

    /* Interrupt handling */
    irqreturn_t (*irq_handler)(struct drm_device *dev);

    /* Debug / info */
    const char *(*get_name)(struct drm_device *dev);
    const char *(*get_desc)(struct drm_device *dev);

    /* Ioctl table — driver-specific ioctls start at DRM_COMMAND_BASE */
    const struct drm_ioctl_desc *ioctls;
    uint32_t                     num_ioctls;
};

/* PCI device ID table */
struct drm_pci_id {
    uint16_t        vendor_id;
    uint16_t        device_id;
    uint16_t        subvendor_id;
    uint16_t        subdevice_id;
    unsigned long   driver_data;
};

/* DRM core ioctls (subset for initial implementation) */
#define DRM_IOCTL_VERSION        0x00
#define DRM_IOCTL_GET_MAGIC      0x01
#define DRM_IOCTL_AUTH_MAGIC     0x02
#define DRM_IOCTL_GET_UNIQUE     0x03
#define DRM_IOCTL_GET_CLIENT     0x04
#define DRM_IOCTL_GET_STATS      0x05
#define DRM_IOCTL_SET_UNIQUE      0x06
#define DRM_IOCTL_SET_MASTER     0x07
#define DRM_IOCTL_DROP_MASTER    0x08

#define DRM_IOCTL_BASE           'd'
#define DRM_IOCTL_NR(n)          _IO(DRM_IOCTL_BASE, n)

#define DRM_COMMAND_BASE         0x40

/* Driver registration */
int  drm_register_driver(const struct drm_driver *driver,
                         const struct drm_pci_id *pci_ids);
void drm_unregister_driver(const struct drm_driver *driver);

/* Device lookup */
drm_device_t *drm_get_device(uint16_t vendor_id, uint16_t device_id);
void          drm_put_device(drm_device_t *dev);

/* Device creation from PCI */
drm_device_t *drm_device_create_from_pci(uint8_t bus, uint8_t dev,
                                         uint8_t func,
                                         const struct drm_driver *driver);

/* DRM subsystem init — creates /dev/dri/card0 */
void drm_init(void);

/* PCI device probe/remove */
int  drm_pci_device_probe(uint8_t bus, uint8_t dev, uint8_t func,
                          const struct drm_driver *driver,
                          const struct drm_pci_id *pci_ids);
void drm_pci_device_remove(uint8_t bus, uint8_t dev, uint8_t func,
                           const struct drm_driver *driver);

/* Ioctl dispatch */
int  drm_ioctl(drm_device_t *dev, unsigned int cmd, unsigned long arg);

#endif /* _DRM_DRIVER_H_ */
