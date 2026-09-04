#include <firmware.h>
#include <kheap.h>
#include <screen.h>
#include <string.h>
#include <vfs.h>
#include <stdint.h>

/* Simple strtoul for hex strings */
static uint32_t simple_strtoul(const char *str, int base) {
    uint32_t result = 0;
    while (*str) {
        char c = *str++;
        uint32_t val = 0;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else break;
        if (val >= (uint32_t)base) break;
        result = result * base + val;
    }
    return result;
}

/* Simple string compare */
static int simple_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++; b++;
    }
    return *a - *b;
}

/* Simple string copy */
static void simple_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++)) {}
}

static int simple_strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *b && *a == *b) {
        a++; b++;
    }
    return n == 0 ? 0 : (*a - *b);
}

static void simple_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static int simple_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

static void simple_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

/* Built-in firmware table - firmware blobs compiled into kernel */
struct builtin_firmware {
    const char *name;
    const uint8_t *data;
    size_t size;
};

/* Example built-in firmware entries (empty data for now - real blobs added per ASIC) */
static const struct builtin_firmware builtin_firmware_table[] = {
    { "amdgpu/polaris10_mc.bin", NULL, 0 },
    { "amdgpu/polaris10_me.bin", NULL, 0 },
    { "amdgpu/polaris10_pfp.bin", NULL, 0 },
    { "amdgpu/polaris10_ce.bin", NULL, 0 },
    { "amdgpu/polaris10_rlc.bin", NULL, 0 },
    { "amdgpu/polaris10_mec.bin", NULL, 0 },
    { "amdgpu/polaris10_sdma.bin", NULL, 0 },
    { "amdgpu/polaris10_sdma1.bin", NULL, 0 },
    { "amdgpu/polaris10_uvd.bin", NULL, 0 },
    { "amdgpu/polaris10_vce.bin", NULL, 0 },
    { "amdgpu/vega10_mc.bin", NULL, 0 },
    { "amdgpu/vega10_me.bin", NULL, 0 },
    { "amdgpu/vega10_pfp.bin", NULL, 0 },
    { "amdgpu/vega10_ce.bin", NULL, 0 },
    { "amdgpu/vega10_rlc.bin", NULL, 0 },
    { "amdgpu/vega10_mec.bin", NULL, 0 },
    { "amdgpu/vega10_sdma.bin", NULL, 0 },
    { "amdgpu/vega10_sdma1.bin", NULL, 0 },
    { "amdgpu/vega10_uvd.bin", NULL, 0 },
    { "amdgpu/vega10_vce.bin", NULL, 0 },
    { "amdgpu/navi10_mc.bin", NULL, 0 },
    { "amdgpu/navi10_me.bin", NULL, 0 },
    { "amdgpu/navi10_pfp.bin", NULL, 0 },
    { "amdgpu/navi10_ce.bin", NULL, 0 },
    { "amdgpu/navi10_rlc.bin", NULL, 0 },
    { "amdgpu/navi10_mec.bin", NULL, 0 },
    { "amdgpu/navi10_sdma.bin", NULL, 0 },
    { "amdgpu/navi10_sdma1.bin", NULL, 0 },
    { "amdgpu/navi10_uvd.bin", NULL, 0 },
    { "amdgpu/navi10_vce.bin", NULL, 0 },
    { NULL, NULL, 0 }
};

static struct firmware_cache_entry *firmware_cache = NULL;

/* Simple CPIO initrd parser for firmware blobs */
static void parse_cpio_initrd(void *initrd_addr, size_t initrd_size) {
    uint8_t *ptr = (uint8_t *)initrd_addr;
    uint8_t *end = ptr + initrd_size;
    
    while (ptr < end) {
        /* Check for CPIO newc format magic "070701" */
        if (ptr + 110 > end) break;
        if (simple_memcmp(ptr, "070701", 6) != 0) {
            /* Try old ASCII format "070707" */
            if (simple_memcmp(ptr, "070707", 6) != 0) break;
        }
        
        /* Parse newc header (110 bytes) */
        char namesize_str[9] = {0};
        char filesize_str[9] = {0};
        
        simple_memcpy(namesize_str, ptr + 94, 8);
        simple_memcpy(filesize_str, ptr + 102, 8);
        
        uint32_t namesize = simple_strtoul(namesize_str, 16);
        uint32_t filesize = simple_strtoul(filesize_str, 16);
        
        ptr += 110;
        
        /* Filename */
        char *name = (char *)ptr;
        if (ptr + namesize > end) break;
        ptr += namesize;
        
        /* Align to 4 bytes */
        ptr = (uint8_t *)(((uint64_t)ptr + 3) & ~3);
        
        /* File data */
        if (ptr + filesize > end) break;
        
        /* Check if it's a firmware file (under /lib/firmware/) */
        if (filesize > 0 && (simple_strncmp(name, "lib/firmware/", 13) == 0 || simple_strncmp(name, "/lib/firmware/", 14) == 0)) {
            const char *fw_name = name + (name[0] == '/' ? 14 : 13);
            
            struct firmware_cache_entry *entry = (struct firmware_cache_entry *)kmalloc(sizeof(struct firmware_cache_entry));
            if (!entry) {
                screen_log("FAIL", COLOR_LIGHT_RED, "Firmware cache: OOM");
                break;
            }
            
            simple_strcpy(entry->name, fw_name);
            entry->name[sizeof(entry->name) - 1] = '\0';
            entry->data = ptr;
            entry->size = filesize;
            entry->next = firmware_cache;
            firmware_cache = entry;
            
            screen_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            screen_print("FW: cached ");
            screen_print(fw_name);
            screen_print(" (");
            char buf[16];
            itoa(filesize, buf, 10);
            screen_print(buf);
            screen_print(" bytes)\n");
        }
        
        ptr += filesize;
        ptr = (uint8_t *)(((uint64_t)ptr + 3) & ~3);
        
        /* Check for TRAILER!!! */
        if (simple_strcmp(name, "TRAILER!!!") == 0) break;
    }
}

void firmware_cache_init(void *initrd_addr, size_t initrd_size) {
    if (!initrd_addr || initrd_size == 0) {
        screen_log("WARN", COLOR_LIGHT_BROWN, "No initrd provided for firmware");
        return;
    }
    
    firmware_cache = NULL;
    parse_cpio_initrd(initrd_addr, initrd_size);
    
    if (firmware_cache) {
        screen_log("OK", COLOR_LIGHT_GREEN, "Firmware cache initialized");
    }
}

/* Search built-in firmware table */
static const struct builtin_firmware *builtin_firmware_find(const char *name) {
    for (int i = 0; builtin_firmware_table[i].name; i++) {
        if (simple_strcmp(builtin_firmware_table[i].name, name) == 0) {
            return &builtin_firmware_table[i];
        }
    }
    return NULL;
}

/* Search ramfs /lib/firmware/ path via VFS */
static const struct firmware_cache_entry *ramfs_firmware_find(const char *name) {
    vfs_node_t *lib = vfs_resolve("/lib");
    if (!lib) return NULL;
    
    vfs_node_t *firmware_dir = vfs_finddir(lib, "firmware");
    if (!firmware_dir) return NULL;
    
    vfs_node_t *fw_node = vfs_finddir(firmware_dir, name);
    if (!fw_node) return NULL;
    
    /* Create a temporary cache entry for this firmware */
    struct firmware_cache_entry *entry = (struct firmware_cache_entry *)kmalloc(sizeof(struct firmware_cache_entry));
    if (!entry) return NULL;
    
    simple_strcpy(entry->name, name);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->size = fw_node->length;
    entry->data = kmalloc(entry->size);
    if (!entry->data) {
        kfree(entry);
        return NULL;
    }
    
    vfs_read(fw_node, 0, entry->size, (uint8_t *)entry->data);
    entry->next = NULL;
    
    return entry;
}

const struct firmware_cache_entry *firmware_cache_find(const char *name) {
    struct firmware_cache_entry *entry = firmware_cache;
    while (entry) {
        if (simple_strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

int request_firmware(const struct firmware **fw, const char *name, void *device) {
    (void)device; /* unused for now */
    
    if (!fw || !name) return -1;
    
    /* Priority 1: Built-in firmware table */
    const struct builtin_firmware *builtin = builtin_firmware_find(name);
    if (builtin && builtin->data && builtin->size > 0) {
        struct firmware *fw_struct = (struct firmware *)kmalloc(sizeof(struct firmware));
        if (!fw_struct) return -1;
        
        fw_struct->size = builtin->size;
        fw_struct->data = builtin->data;
        *fw = fw_struct;
        
        screen_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        screen_print("FW: loaded builtin ");
        screen_print(name);
        screen_print(" (");
        char buf[16];
        itoa(builtin->size, buf, 10);
        screen_print(buf);
        screen_print(" bytes)\n");
        
        return 0;
    }
    
    /* Priority 2: Initrd CPIO cache */
    const struct firmware_cache_entry *entry = firmware_cache_find(name);
    if (entry) {
        struct firmware *fw_struct = (struct firmware *)kmalloc(sizeof(struct firmware));
        if (!fw_struct) return -1;
        
        fw_struct->size = entry->size;
        fw_struct->data = entry->data;
        *fw = fw_struct;
        
        screen_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        screen_print("FW: loaded from initrd ");
        screen_print(name);
        screen_print(" (");
        char buf[16];
        itoa(entry->size, buf, 10);
        screen_print(buf);
        screen_print(" bytes)\n");
        
        return 0;
    }
    
    /* Priority 3: Ramfs /lib/firmware/ */
    const struct firmware_cache_entry *ramfs_entry = ramfs_firmware_find(name);
    if (ramfs_entry) {
        struct firmware *fw_struct = (struct firmware *)kmalloc(sizeof(struct firmware));
        if (!fw_struct) {
            kfree((void *)ramfs_entry->data);
            kfree((void *)ramfs_entry);
            return -1;
        }
        
        fw_struct->size = ramfs_entry->size;
        fw_struct->data = ramfs_entry->data;
        *fw = fw_struct;
        
        screen_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        screen_print("FW: loaded from ramfs ");
        screen_print(name);
        screen_print(" (");
        char buf[16];
        itoa(ramfs_entry->size, buf, 10);
        screen_print(buf);
        screen_print(" bytes)\n");
        
        kfree((void *)ramfs_entry);
        return 0;
    }
    
    /* Not found */
    screen_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    screen_print("FW: not found: ");
    screen_print(name);
    screen_print("\n");
    return -1; /* ENOENT */
}

void release_firmware(const struct firmware *fw) {
    if (fw) {
        kfree((void *)fw);
    }
}

/* Register built-in firmware blob (called by driver during init) */
int register_builtin_firmware(const char *name, const uint8_t *data, size_t size) {
    for (int i = 0; builtin_firmware_table[i].name; i++) {
        if (simple_strcmp(builtin_firmware_table[i].name, name) == 0) {
            /* Can't modify const table at runtime - this is a placeholder for future use */
            (void)data; (void)size;
            return 0;
        }
    }
    return -1;
}