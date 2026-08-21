#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdint.h>
#include <stddef.h>

struct firmware {
    size_t size;
    const uint8_t *data;
};

int request_firmware(const struct firmware **fw, const char *name, void *device);
void release_firmware(const struct firmware *fw);

/* Internal: firmware cache for initrd-loaded blobs */
struct firmware_cache_entry {
    char name[64];
    const uint8_t *data;
    size_t size;
    struct firmware_cache_entry *next;
};

void firmware_cache_init(void *initrd_addr, size_t initrd_size);
const struct firmware_cache_entry *firmware_cache_find(const char *name);

/* Register built-in firmware blob (called by driver during init) */
int register_builtin_firmware(const char *name, const uint8_t *data, size_t size);

#endif