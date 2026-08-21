#include <fat32.h>
#include <ata.h>
#include <kheap.h>
#include <screen.h>
#include <vfs.h>
#include <stddef.h>

struct fat32_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_time_ten;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed));

/* VFAT long filename entry — 32 bytes, attr must be 0x0F */
struct fat32_lfn {
    uint8_t  seq;          /* 0x40|N for the last part, N otherwise */
    uint16_t chars0_4[5];  /* bytes 01-10: characters 0..4 */
    uint8_t  attr;         /* 0x0F */
    uint8_t  type;         /* 0x00 */
    uint8_t  checksum;     /* checksum of the short name */
    uint16_t chars5_10[6]; /* bytes 14-25: characters 5..10 */
    uint16_t zero;         /* bytes 26-27: must be 0x0000 */
    uint16_t chars11_12[2];/* bytes 28-31: characters 11..12 */
} __attribute__((packed));

static uint32_t sectors_per_cluster = 0;
static uint32_t reserved_sectors    = 0;
static uint32_t num_fats            = 0;
static uint32_t fat_sz_32           = 0;
static uint32_t root_clus           = 0;
static uint32_t data_sector         = 0;
static uint32_t lba_start           = 0;
static uint8_t  fat32_ready         = 0;

static uint32_t cluster_to_sector(uint32_t clus) {
    return data_sector + (clus - 2) * sectors_per_cluster;
}

/* ---- FAT helpers ---------------------------------------------------------- */

static int fat_read_entry(uint32_t clus, uint32_t *out_val) {
    uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
    uint32_t fat_off = (clus * 4) % 512;
    uint8_t fat_buf[512];
    if (ata_read_sector(fat_sec, fat_buf) != 0) return -1;
    *out_val = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
    return 0;
}

static int fat_write_entry(uint32_t clus, uint32_t val) {
    uint8_t fat_buf[512];
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_sec = lba_start + reserved_sectors + f * fat_sz_32 + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return -1;
        *(uint32_t *)(fat_buf + fat_off) = (val & 0x0FFFFFFF) | (*(uint32_t *)(fat_buf + fat_off) & 0xF0000000);
        if (ata_write_sector(fat_sec, fat_buf) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_alloc_cluster(void) {
    uint8_t fat_buf[512];
    uint32_t total_clusters = (fat_sz_32 * 512) / 4;
    
    for (uint32_t clus = 2; clus < total_clusters; clus++) {
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) continue;
        uint32_t val = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
        if (val == 0) {
            if (fat_write_entry(clus, 0x0FFFFFFF) == 0) {
                return clus;
            }
        }
    }
    return 0;
}

static int fat_free_chain(uint32_t start_clus) {
    uint32_t clus = start_clus;
    while (clus < 0x0FFFFFF8 && clus != 0) {
        uint32_t next = 0;
        if (fat_read_entry(clus, &next) != 0) return -1;
        if (fat_write_entry(clus, 0) != 0) return -1;
        clus = next;
    }
    return 0;
}

static uint32_t fat_get_last_cluster(uint32_t start_clus) {
    uint32_t clus = start_clus;
    uint32_t prev = clus;
    while (clus < 0x0FFFFFF8 && clus != 0) {
        prev = clus;
        uint32_t next = 0;
        if (fat_read_entry(clus, &next) != 0) break;
        clus = next;
    }
    return prev;
}

static uint32_t fat_extend_chain(uint32_t start_clus, uint32_t needed_clusters) {
    uint32_t last = fat_get_last_cluster(start_clus);
    uint32_t clus = last;
    
    for (uint32_t i = 0; i < needed_clusters; i++) {
        uint32_t new_clus = fat_alloc_cluster();
        if (!new_clus) return 0;
        if (fat_write_entry(clus, new_clus) != 0) return 0;
        if (fat_write_entry(new_clus, 0x0FFFFFFF) != 0) return 0;
        clus = new_clus;
    }
    return clus;
}

/* ---- 8.3 name conversion --------------------------------------------------- */

static void fat_name_to_normal(const uint8_t *fat_name, char *out) {
    int idx = 0;
    for (int i = 0; i < 8; i++) {
        if (fat_name[i] != ' ') {
            char ch = fat_name[i];
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            out[idx++] = ch;
        }
    }
    if (fat_name[8] != ' ') {
        out[idx++] = '.';
        for (int i = 8; i < 11; i++) {
            if (fat_name[i] != ' ') {
                char ch = fat_name[i];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                out[idx++] = ch;
            }
        }
    }
    out[idx] = '\0';
}

static void normal_to_fat_name(const char *name, uint8_t *fat_name) {
    for (int i = 0; i < 11; i++) fat_name[i] = ' ';
    
    int name_len = 0;
    while (name[name_len] && name_len < 127) name_len++;
    
    int dot_pos = -1;
    for (int i = 0; i < name_len; i++) {
        if (name[i] == '.') { dot_pos = i; break; }
    }
    
    if (dot_pos == -1) {
        for (int i = 0; i < name_len && i < 8; i++) {
            char ch = name[i];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            fat_name[i] = ch;
        }
    } else {
        for (int i = 0; i < dot_pos && i < 8; i++) {
            char ch = name[i];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            fat_name[i] = ch;
        }
        for (int i = dot_pos + 1, j = 8; i < name_len && j < 11; i++, j++) {
            char ch = name[i];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            fat_name[j] = ch;
        }
    }
}

/* ---- VFAT long filename helpers ------------------------------------------- */

/* Checksum algorithm used by VFAT to tie LFN entries to their short name */
static uint8_t lfn_checksum(const uint8_t *short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) << 7) | (sum >> 1);
        sum += short_name[i];
    }
    return sum;
}

/* Accumulator for LFN parts.  Parts are stored on disk in reverse order:
 * the part with sequence 0x40|N comes first, then N-1, ... 1, and finally
 * the short entry. */
typedef struct {
    uint16_t chars[256];
    int      first_seq;  /* 0x40|N part sequence or 0 if none seen */
    uint8_t  checksum;
} lfn_acc_t;

static void lfn_reset(lfn_acc_t *acc) {
    acc->first_seq = 0;
    acc->checksum  = 0;
}

static void lfn_accumulate(lfn_acc_t *acc, const struct fat32_lfn *lfn) {
    int seq = lfn->seq & 0x1F;
    if (seq < 1 || seq > 20) return;

    if (acc->first_seq == 0) {
        /* First part we see is the last logical part (0x40|N) */
        if (!(lfn->seq & 0x40)) return;
        acc->first_seq = seq;
        acc->checksum  = lfn->checksum;
    } else {
        /* Subsequent parts must belong to the same group */
        if (seq >= acc->first_seq || lfn->checksum != acc->checksum) {
            lfn_reset(acc);
            return;
        }
    }

    int base = (seq - 1) * 13;
    for (int i = 0; i < 5; i++) acc->chars[base + i]      = lfn->chars0_4[i];
    for (int i = 0; i < 6; i++) acc->chars[base + 5 + i]  = lfn->chars5_10[i];
    for (int i = 0; i < 2; i++) acc->chars[base + 11 + i] = lfn->chars11_12[i];
}

static int lfn_verify(const lfn_acc_t *acc, const uint8_t *short_name) {
    if (!acc->first_seq) return 0;
    return acc->checksum == lfn_checksum(short_name);
}

static void lfn_build_name(const lfn_acc_t *acc, char *out, int max) {
    int end = (acc->first_seq - 1) * 13;
    while (end < (acc->first_seq - 1) * 13 + 13 && end < 256) {
        uint16_t c = acc->chars[end];
        if (c == 0x0000 || c == 0xFFFF) break;
        end++;
    }
    int o = 0;
    for (int i = 0; i < end && o < max - 1; i++) {
        uint16_t c = acc->chars[i];
        out[o++] = (c < 0x80) ? (char)c : '?';
    }
    out[o] = '\0';
}

static int valid_char_ptr(char c, const char *valid) {
    const char *p = valid;
    while (*p) {
        if (*p++ == c) return 1;
    }
    return 0;
}

/* Does this name need an LFN entry (doesn't fit an uppercase 8.3 name)? */
static int name_needs_lfn(const char *name) {
    static const char *valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_^$~!#%&-{}()@'`";

    int len = 0;
    while (name[len] && len < 255) len++;

    int dot = -1;
    for (int i = 0; i < len; i++) {
        if (name[i] == '.') { dot = i; break; }
    }

    int base_len = (dot == -1) ? len : dot;
    int ext_len  = (dot == -1) ? 0 : len - dot - 1;

    if (base_len > 8 || ext_len > 3) return 1;
    if (dot != -1 && ext_len == 0) return 1;
    if (base_len == 0 || len == 0) return 1;

    for (int i = 0; i < base_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') return 1;
        if (c == '.') return 1;
        if (!valid_char_ptr(c, valid)) return 1;
    }
    for (int i = dot + 1; i < len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') return 1;
        if (!valid_char_ptr(c, valid)) return 1;
    }
    return 0;
}

static int name_to_utf16(const char *name, uint16_t *out, int max) {
    int len = 0;
    while (name[len] && len < max) {
        uint8_t c = (uint8_t)name[len];
        out[len] = (c < 0x80) ? c : '?';
        len++;
    }
    return len;
}

/* ---- Directory scan helpers ------------------------------------------------ */

static int fat32_short_name_exists(vfs_node_t *dir_node, const uint8_t *fat_name);

/* Build a unique 8.3 short name for `name` inside `dir_node`.
 * Falls back to a "XXXXXX~N" style name on collision. */
static void build_short_name(vfs_node_t *dir_node, const char *name,
                             uint8_t *fat_name, uint8_t *sum_out) {
    uint8_t base[8];
    uint8_t ext[3];

    normal_to_fat_name(name, fat_name);
    for (int i = 0; i < 8; i++) base[i] = fat_name[i];
    for (int i = 0; i < 3; i++) ext[i] = fat_name[8 + i];

    int base_len = 8;
    while (base_len > 0 && base[base_len - 1] == ' ') base_len--;

    uint32_t try_num = 0;
    while (fat32_short_name_exists(dir_node, fat_name)) {
        if (++try_num > 99) break;

        /* "XXXXXX~N" — first 6 base chars + '~' + decimal N */
        for (int i = 0; i < 8; i++) fat_name[i] = ' ';
        int keep = (base_len < 6) ? base_len : 6;
        for (int i = 0; i < keep; i++) fat_name[i] = base[i];

        char num[4];
        int nl = 0;
        uint32_t n = try_num;
        while (n) { num[nl++] = '0' + (n % 10); n /= 10; }

        if (keep + 1 + nl <= 8) {
            fat_name[keep] = '~';
            for (int i = 0; i < nl; i++) fat_name[keep + 1 + i] = num[nl - 1 - i];
        } else {
            break;
        }
        for (int i = 0; i < 3; i++) fat_name[8 + i] = ext[i];
    }

    if (sum_out) *sum_out = lfn_checksum(fat_name);
}

/* Check whether an 11-byte short name already exists in a directory */
static int fat32_short_name_exists(vfs_node_t *dir_node, const uint8_t *fat_name) {
    uint32_t clus = dir_node->inode;
    uint8_t sector_buf[512], fat_buf[512];

    while (clus < 0x0FFFFFF8) {
        uint32_t sector_start = cluster_to_sector(clus);
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (ata_read_sector(sector_start + sec, sector_buf) != 0) return 1;
            for (int e = 0; e < 16; e++) {
                struct fat32_dirent *entry = (struct fat32_dirent *)(sector_buf + e * 32);
                if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) continue;
                if (entry->attr == 0x0F) continue;
                int same = 1;
                for (int i = 0; i < 11; i++) {
                    if (entry->name[i] != fat_name[i]) { same = 0; break; }
                }
                if (same) return 1;
            }
        }
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return 1;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
    }
    return 0;
}

/* Case-insensitive ASCII compare of two names */
static int names_equal(const char *a, const char *b) {
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
        i++;
    }
}

/* Locate a directory entry by its full name (LFN aware).
 * Returns 0 and fills the entry's sector LBA, index and 8.3 short name. */
static int fat32_find_entry(vfs_node_t *dir_node, const char *name,
                            uint32_t *out_sector, int *out_index,
                            uint8_t *out_short_name) {
    uint32_t clus = dir_node->inode;
    uint8_t sector_buf[512], fat_buf[512];
    lfn_acc_t acc;
    lfn_reset(&acc);

    while (clus < 0x0FFFFFF8) {
        uint32_t sector_start = cluster_to_sector(clus);
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (ata_read_sector(sector_start + sec, sector_buf) != 0) return -1;
            for (int e = 0; e < 16; e++) {
                struct fat32_dirent *entry = (struct fat32_dirent *)(sector_buf + e * 32);
                if (entry->name[0] == 0x00) return -1;  /* end of directory */
                if (entry->name[0] == 0xE5) { lfn_reset(&acc); continue; }
                if (entry->attr == 0x0F) {
                    lfn_accumulate(&acc, (struct fat32_lfn *)entry);
                    continue;
                }

                char full_name[VFS_MAX_NAME];
                if (lfn_verify(&acc, entry->name)) {
                    lfn_build_name(&acc, full_name, sizeof(full_name));
                } else {
                    fat_name_to_normal(entry->name, full_name);
                }
                lfn_reset(&acc);

                if (!names_equal(name, full_name)) continue;

                if (out_sector) *out_sector = sector_start + sec;
                if (out_index)  *out_index  = e;
                if (out_short_name) {
                    for (int i = 0; i < 11; i++) out_short_name[i] = entry->name[i];
                }
                return 0;
            }
        }
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return -1;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
    }
    return -1;
}

/* Reserve a run of `count` consecutive free entries inside a single
 * directory sector.  Grows the directory with a fresh cluster if needed. */
static int fat32_dir_reserve(vfs_node_t *dir_node, int count,
                             uint32_t *out_sector, int *out_index) {
    uint32_t clus = dir_node->inode;
    uint8_t sector_buf[512], fat_buf[512];

    while (clus < 0x0FFFFFF8) {
        uint32_t sector_start = cluster_to_sector(clus);
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (ata_read_sector(sector_start + sec, sector_buf) != 0) return -1;
            int run = 0, run_start = 0;
            for (int e = 0; e < 16; e++) {
                struct fat32_dirent *entry = (struct fat32_dirent *)(sector_buf + e * 32);
                if (entry->name[0] == 0xE5 || entry->name[0] == 0x00) {
                    if (run == 0) run_start = e;
                    run++;
                } else {
                    run = 0;
                }
                if (run >= count) {
                    *out_sector = sector_start + sec;
                    *out_index  = run_start;
                    return 0;
                }
            }
        }
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return -1;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
    }

    /* No room — append one zeroed cluster to the directory */
    uint32_t last = fat_get_last_cluster(dir_node->inode);
    if (!last) return -1;
    uint32_t new_clus = fat_alloc_cluster();
    if (!new_clus) return -1;
    if (fat_write_entry(last, new_clus) != 0) return -1;
    if (fat_write_entry(new_clus, 0x0FFFFFFF) != 0) return -1;

    uint8_t zero_buf[512];
    for (int i = 0; i < 512; i++) zero_buf[i] = 0;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (ata_write_sector(cluster_to_sector(new_clus) + s, zero_buf) != 0) return -1;
    }

    *out_sector = cluster_to_sector(new_clus);
    *out_index  = 0;
    return 0;
}

/* Write one 32-byte entry at (sector, index) */
static int fat32_write_entry_at(uint32_t sector, int index, const uint8_t *raw_entry) {
    uint8_t sector_buf[512];
    if (ata_read_sector(sector, sector_buf) != 0) return -1;
    for (int i = 0; i < 32; i++) sector_buf[index * 32 + i] = raw_entry[i];
    return ata_write_sector(sector, sector_buf);
}

/* Fill a 32-byte LFN slot (used by the writer) */
static void fat32_fill_lfn_slot(uint8_t *slot, int seq, int is_last,
                                const uint16_t *name16, int name_len,
                                int part_base, uint8_t checksum) {
    struct fat32_lfn *lfn = (struct fat32_lfn *)slot;
    uint16_t u16[13];
    for (int i = 0; i < 32; i++) slot[i] = 0;
    for (int i = 0; i < 13; i++) u16[i] = 0xFFFF;

    int src = part_base;
    int n = 0;
    while (n < 13 && src < name_len) u16[n++] = name16[src++];
    if (src == name_len && name_len - part_base < 13) u16[n] = 0x0000;

    lfn->seq       = (uint8_t)((is_last ? 0x40 : 0) | seq);
    lfn->attr      = 0x0F;
    lfn->checksum  = checksum;
    for (int i = 0; i < 5; i++) lfn->chars0_4[i] = u16[i];
    for (int i = 0; i < 6; i++) lfn->chars5_10[i] = u16[5 + i];
    lfn->zero       = 0;
    for (int i = 0; i < 2; i++) lfn->chars11_12[i] = u16[11 + i];
}

/* Fill a 32-byte 8.3 short entry slot */
static void fat32_fill_short_slot(uint8_t *slot, const uint8_t *short_name,
                                  uint32_t first_cluster, uint32_t file_size,
                                  uint8_t attr) {
    struct fat32_dirent *entry = (struct fat32_dirent *)slot;
    for (int i = 0; i < 32; i++) slot[i] = 0;
    for (int i = 0; i < 11; i++) entry->name[i] = short_name[i];
    entry->attr       = attr;
    entry->ntres      = 0;
    entry->crt_time_ten = 0;
    entry->crt_time   = 0;
    entry->crt_date   = 0;
    entry->lst_acc_date = 0;
    entry->fst_clus_hi = (first_cluster >> 16) & 0xFFFF;
    entry->wrt_time   = 0;
    entry->wrt_date   = 0;
    entry->fst_clus_lo = first_cluster & 0xFFFF;
    entry->file_size  = file_size;
}

/* Read a directory entry's full name. Returns 0 and fills `out`. */
static void fat32_entry_full_name(const struct fat32_dirent *entry,
                                  const lfn_acc_t *acc, char *out, int max) {
    if (lfn_verify(acc, entry->name)) {
        lfn_build_name(acc, out, max);
    } else {
        fat_name_to_normal(entry->name, out);
    }
}

/* ---- File read ---------------------------------------------------------- */

uint32_t fat32_file_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    if (!fat32_ready) return 0;

    uint32_t file_size = node->length;
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint32_t clus = node->inode;
    uint32_t bytes_per_cluster = sectors_per_cluster * 512;
    uint32_t skip_clusters = offset / bytes_per_cluster;

    uint8_t fat_buf[512];
    for (uint32_t i = 0; i < skip_clusters; i++) {
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return 0;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
        if (clus >= 0x0FFFFFF8) return 0;
    }

    uint32_t cluster_offset = offset % bytes_per_cluster;
    uint32_t bytes_read = 0;
    uint8_t cluster_buf[512];

    while (bytes_read < size) {
        uint32_t sector_start = cluster_to_sector(clus);

        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            uint32_t sec_offset = sec * 512;
            if (cluster_offset >= sec_offset + 512) continue;

            if (ata_read_sector(sector_start + sec, cluster_buf) != 0) return bytes_read;

            uint32_t copy_start = (cluster_offset > sec_offset) ? (cluster_offset - sec_offset) : 0;
            uint32_t copy_len   = 512 - copy_start;
            if (copy_len > (size - bytes_read)) copy_len = size - bytes_read;

            for (uint32_t m = 0; m < copy_len; m++)
                buf[bytes_read++] = cluster_buf[copy_start + m];

            if (bytes_read >= size) break;
        }
        if (bytes_read >= size) break;

        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) break;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
        if (clus >= 0x0FFFFFF8) break;
        cluster_offset = 0;
    }
    return bytes_read;
}

/* ---- Directory entry creation/update -------------------------------------- */

static int fat32_write_dirent(vfs_node_t *dir_node, const char *name,
                              uint32_t first_cluster, uint32_t file_size,
                              uint8_t attr, uint8_t *out_short_name) {
    uint8_t short_name[11];
    uint32_t sector;
    int index;

    /* Entry already exists?  Just refresh size/first-cluster. */
    if (fat32_find_entry(dir_node, name, &sector, &index, short_name) == 0) {
        uint8_t slot[32];
        fat32_fill_short_slot(slot, short_name, first_cluster, file_size, attr);
        if (fat32_write_entry_at(sector, index, slot) != 0) return -1;
        if (out_short_name) {
            for (int i = 0; i < 11; i++) out_short_name[i] = short_name[i];
        }
        return 0;
    }

    /* New entry: unique short name + optional LFN group */
    uint8_t checksum;
    build_short_name(dir_node, name, short_name, &checksum);

    uint16_t name16[256];
    int name_len = name_to_utf16(name, name16, 256);

    int lfn_count = 0;
    if (name_needs_lfn(name)) lfn_count = (name_len + 12) / 13;
    if (lfn_count == 0) name_len = 0;  /* don't write an unnecessary LFN */

    if (fat32_dir_reserve(dir_node, 1 + lfn_count, &sector, &index) != 0) return -1;

    for (int p = lfn_count; p >= 1; p--) {
        uint8_t slot[32];
        fat32_fill_lfn_slot(slot, p, p == lfn_count, name16, name_len,
                            (p - 1) * 13, checksum);
        if (fat32_write_entry_at(sector, index++, slot) != 0) return -1;
    }

    uint8_t slot[32];
    fat32_fill_short_slot(slot, short_name, first_cluster, file_size, attr);
    if (fat32_write_entry_at(sector, index, slot) != 0) return -1;

    if (out_short_name) {
        for (int i = 0; i < 11; i++) out_short_name[i] = short_name[i];
    }
    return 0;
}

static int fat32_update_dirent(vfs_node_t *dir_node, const char *name,
                               uint32_t first_cluster, uint32_t file_size,
                               uint8_t attr) {
    return fat32_write_dirent(dir_node, name, first_cluster, file_size, attr, NULL);
}

/* ---- File write ----------------------------------------------------------- */

uint32_t fat32_file_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    if (!fat32_ready) return 0;
    
    uint32_t bytes_per_cluster = sectors_per_cluster * 512;
    uint32_t file_size = node->length;
    
    /* Handle append/truncate */
    if (offset > file_size) {
        /* Seek past EOF - zero fill */
        offset = file_size;
    }
    
    uint32_t new_size = offset + size;
    uint32_t clus = node->inode;
    
    /* If file has no clusters yet, allocate first one */
    if (clus == 0) {
        clus = fat_alloc_cluster();
        if (!clus) return 0;
        node->inode = clus;
    }
    
    /* Calculate how many clusters we need */
    uint32_t clusters_needed = (new_size + bytes_per_cluster - 1) / bytes_per_cluster;
    uint32_t current_clusters = (file_size + bytes_per_cluster - 1) / bytes_per_cluster;
    
    if (clusters_needed > current_clusters) {
        if (!fat_extend_chain(clus, clusters_needed - current_clusters)) return 0;
    } else if (clusters_needed < current_clusters) {
        /* Truncate - free excess clusters */
        uint32_t keep_clus = clus;
        for (uint32_t i = 0; i < clusters_needed - 1; i++) {
            uint32_t next = 0;
            if (fat_read_entry(keep_clus, &next) != 0) break;
            keep_clus = next;
        }
        uint32_t free_start = 0;
        if (fat_read_entry(keep_clus, &free_start) == 0) {
            fat_write_entry(keep_clus, 0x0FFFFFFF);
            fat_free_chain(free_start);
        }
    }
    
    /* Write data */
    uint32_t skip_clusters = offset / bytes_per_cluster;
    uint8_t fat_buf[512];
    for (uint32_t i = 0; i < skip_clusters; i++) {
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return 0;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
        if (clus >= 0x0FFFFFF8) return 0;
    }
    
    uint32_t cluster_offset = offset % bytes_per_cluster;
    uint32_t bytes_written = 0;
    uint8_t cluster_buf[512];
    
    while (bytes_written < size) {
        uint32_t sector_start = cluster_to_sector(clus);
        
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            uint32_t sec_offset = sec * 512;
            if (cluster_offset >= sec_offset + 512) continue;
            
            if (ata_read_sector(sector_start + sec, cluster_buf) != 0) return bytes_written;
            
            uint32_t copy_start = (cluster_offset > sec_offset) ? (cluster_offset - sec_offset) : 0;
            uint32_t copy_len = 512 - copy_start;
            if (copy_len > (size - bytes_written)) copy_len = size - bytes_written;
            
            for (uint32_t m = 0; m < copy_len; m++)
                cluster_buf[copy_start + m] = buf[bytes_written++];
            
            if (ata_write_sector(sector_start + sec, cluster_buf) != 0) return bytes_written;
            
            if (bytes_written >= size) break;
        }
        if (bytes_written >= size) break;
        
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) break;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
        if (clus >= 0x0FFFFFF8) break;
        cluster_offset = 0;
    }
    
    /* Update file size in node */
    if (new_size > node->length) {
        node->length = new_size;
        /* Update directory entry */
        vfs_node_t *parent = vfs_resolve("/fat32");  /* This is a hack - we need proper parent tracking */
        if (parent) {
            fat32_update_dirent(parent, node->name, node->inode, node->length, 0x20);
        }
    }
    
    return bytes_written;
}

/* ---- Directory entry creation/deletion ------------------------------------ */

static int fat32_create_dirent(vfs_node_t *dir_node, const char *name, uint8_t attr, uint32_t *out_first_cluster) {
    uint32_t first_cluster = 0;
    if (attr & 0x10) {
        /* Directory - allocate a cluster for it */
        first_cluster = fat_alloc_cluster();
        if (!first_cluster) return -1;
        /* Initialize directory cluster with . and .. entries */
        uint8_t cluster_buf[512];
        for (uint32_t i = 0; i < sectors_per_cluster * 512; i++) cluster_buf[i] = 0;
        
        /* . entry */
        struct fat32_dirent *dot = (struct fat32_dirent *)cluster_buf;
        normal_to_fat_name(".", dot->name);
        dot->attr = 0x10;
        dot->fst_clus_hi = (first_cluster >> 16) & 0xFFFF;
        dot->fst_clus_lo = first_cluster & 0xFFFF;
        dot->file_size = 0;
        
        /* .. entry */
        struct fat32_dirent *dotdot = (struct fat32_dirent *)(cluster_buf + 32);
        normal_to_fat_name("..", dotdot->name);
        dotdot->attr = 0x10;
        dotdot->fst_clus_hi = (dir_node->inode >> 16) & 0xFFFF;
        dotdot->fst_clus_lo = dir_node->inode & 0xFFFF;
        dotdot->file_size = 0;
        
        uint32_t sector_start = cluster_to_sector(first_cluster);
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (ata_write_sector(sector_start + sec, cluster_buf) != 0) return -1;
        }
    }
    
    if (fat32_write_dirent(dir_node, name, first_cluster, 0, attr, NULL) != 0) {
        if (first_cluster) fat_free_chain(first_cluster);
        return -1;
    }
    
    if (out_first_cluster) *out_first_cluster = first_cluster;
    return 0;
}

int fat32_create_file(vfs_node_t *dir_node, const char *name) {
    return fat32_create_dirent(dir_node, name, 0x20, NULL);
}

int fat32_create_dir(vfs_node_t *dir_node, const char *name) {
    return fat32_create_dirent(dir_node, name, 0x10, NULL);
}

int fat32_delete_entry(vfs_node_t *dir_node, const char *name) {
    uint8_t short_name[11];
    uint32_t sector;
    int index;

    if (fat32_find_entry(dir_node, name, &sector, &index, short_name) != 0)
        return -1;

    uint8_t sector_buf[512];
    if (ata_read_sector(sector, sector_buf) != 0) return -1;

    /* Free the entry's cluster chain first */
    struct fat32_dirent *entry = (struct fat32_dirent *)(sector_buf + index * 32);
    if (!(entry->attr & 0x10)) {
        uint32_t file_clus = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
        if (file_clus) fat_free_chain(file_clus);
    } else {
        uint32_t dir_clus = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
        if (dir_clus) fat_free_chain(dir_clus);
    }

    /* Mark the LFN group (if any) and the short entry as deleted.
     * LFN parts always sit immediately before the short entry. */
    uint8_t sum = lfn_checksum(short_name);
    for (int e = index - 1; e >= 0; e--) {
        struct fat32_lfn *lfn = (struct fat32_lfn *)(sector_buf + e * 32);
        if (lfn->attr != 0x0F) break;
        if (lfn->checksum != sum) break;
        sector_buf[e * 32] = 0xE5;
    }
    sector_buf[index * 32] = 0xE5;

    if (ata_write_sector(sector, sector_buf) != 0) return -1;
    return 0;
}

/* ---- Directory helpers --------------------------------------------------- */

vfs_dirent_t *fat32_readdir(vfs_node_t *node, uint32_t index) {
    if (!fat32_ready) return NULL;
    static vfs_dirent_t dirent;
    uint32_t clus = node->inode;
    uint32_t current_idx = 0;
    uint8_t sector_buf[512], fat_buf[512];
    lfn_acc_t acc;
    lfn_reset(&acc);

    while (clus < 0x0FFFFFF8) {
        uint32_t sector_start = cluster_to_sector(clus);
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (ata_read_sector(sector_start + sec, sector_buf) != 0) return NULL;
            for (int e = 0; e < 16; e++) {
                struct fat32_dirent *entry = (struct fat32_dirent *)(sector_buf + e * 32);
                if (entry->name[0] == 0x00) return NULL;
                if (entry->name[0] == 0xE5) { lfn_reset(&acc); continue; }
                if (entry->attr == 0x0F) {
                    lfn_accumulate(&acc, (struct fat32_lfn *)entry);
                    continue;
                }
                if (current_idx == index) {
                    char full_name[VFS_MAX_NAME];
                    fat32_entry_full_name(entry, &acc, full_name, sizeof(full_name));
                    lfn_reset(&acc);
                    int i = 0;
                    while (full_name[i] && i < VFS_MAX_NAME - 1) {
                        dirent.name[i] = full_name[i];
                        i++;
                    }
                    dirent.name[i] = '\0';
                    dirent.inode = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
                    return &dirent;
                }
                lfn_reset(&acc);
                current_idx++;
            }
        }
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return NULL;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
    }
    return NULL;
}

vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name) {
    if (!fat32_ready) return NULL;
    uint32_t clus = node->inode;
    uint8_t sector_buf[512], fat_buf[512];
    char normal_name[128];
    lfn_acc_t acc;
    lfn_reset(&acc);

    while (clus < 0x0FFFFFF8) {
        uint32_t sector_start = cluster_to_sector(clus);
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (ata_read_sector(sector_start + sec, sector_buf) != 0) return NULL;
            for (int e = 0; e < 16; e++) {
                struct fat32_dirent *entry = (struct fat32_dirent *)(sector_buf + e * 32);
                if (entry->name[0] == 0x00) return NULL;
                if (entry->name[0] == 0xE5) { lfn_reset(&acc); continue; }
                if (entry->attr == 0x0F) {
                    lfn_accumulate(&acc, (struct fat32_lfn *)entry);
                    continue;
                }

                fat32_entry_full_name(entry, &acc, normal_name, sizeof(normal_name));
                lfn_reset(&acc);

                if (!names_equal(name, normal_name)) continue;

                vfs_node_t *child = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
                if (!child) return NULL;

                int m = 0;
                while (normal_name[m] && m < VFS_MAX_NAME - 1) {
                    child->name[m] = normal_name[m];
                    m++;
                }
                child->name[m] = '\0';

                child->inode = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
                child->ptr   = NULL;
                child->uid   = child->gid = 0;

                if (entry->attr & 0x10) {
                    child->flags   = VFS_DIRECTORY;
                    child->length  = 0;
                    child->read    = NULL; child->write = NULL;
                    child->open    = NULL; child->close = NULL;
                    child->finddir = fat32_finddir;
                    child->readdir = fat32_readdir;
                } else {
                    child->flags   = VFS_FILE;
                    child->length  = entry->file_size;
                    child->read    = fat32_file_read;
                    child->write   = fat32_file_write;
                    child->open    = NULL; child->close = NULL;
                    child->finddir = NULL; child->readdir = NULL;
                }
                return child;
            }
        }
        uint32_t fat_sec = lba_start + reserved_sectors + (clus * 4) / 512;
        uint32_t fat_off = (clus * 4) % 512;
        if (ata_read_sector(fat_sec, fat_buf) != 0) return NULL;
        clus = (*(uint32_t *)(fat_buf + fat_off)) & 0x0FFFFFFF;
    }
    return NULL;
}

/* ---- Mount --------------------------------------------------------------- */

int fat32_init_and_mount(vfs_node_t *mount_node) {
    if (!mount_node) return -1;

    /* Detect IDE disk first */
    ata_detect();

    uint8_t boot_sec[512];

    /* Try sector 0 - could be MBR or FAT32 VBR directly */
    if (ata_read_sector(0, boot_sec) != 0) {
        screen_log("WARN", COLOR_LIGHT_BROWN, "FAT32: Nenhum disco IDE detectado, montagem ignorada.");
        return -1;
    }

    lba_start = 0;
    uint16_t bps = *(uint16_t *)(boot_sec + 11);

    /* If sector 0 is an MBR, get the first partition's LBA start */
    if (bps != 512) {
        lba_start = *(uint32_t *)(boot_sec + 0x1C6);
        if (ata_read_sector(lba_start, boot_sec) != 0) {
            screen_log("FAIL", COLOR_LIGHT_RED, "FAT32: Falha ao ler VBR da particao.");
            return -1;
        }
        bps = *(uint16_t *)(boot_sec + 11);
    }

    if (bps != 512) {
        screen_log("FAIL", COLOR_LIGHT_RED, "FAT32: VBR invalida (bytes_per_sector != 512).");
        return -1;
    }

    sectors_per_cluster = boot_sec[13];
    reserved_sectors    = *(uint16_t *)(boot_sec + 14);
    num_fats            = boot_sec[16];
    fat_sz_32           = *(uint32_t *)(boot_sec + 36);
    root_clus           = *(uint32_t *)(boot_sec + 44);
    data_sector         = lba_start + reserved_sectors + (num_fats * fat_sz_32);
    fat32_ready         = 1;

    screen_log(" OK ", COLOR_LIGHT_GREEN, "FAT32: Disco montado em /fat32");

    mount_node->flags   = VFS_DIRECTORY;
    mount_node->inode   = root_clus;
    mount_node->length  = 0;
    mount_node->read    = NULL; mount_node->write = NULL;
    mount_node->open    = NULL; mount_node->close = NULL;
    mount_node->finddir = fat32_finddir;
    mount_node->readdir = fat32_readdir;
    mount_node->ptr     = NULL;
    return 0;
}