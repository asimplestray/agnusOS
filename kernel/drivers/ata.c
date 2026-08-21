#include <ata.h>
#include <bcache.h>
#include <io.h>

/* ---- ATA register definitions ------------------------------------------- */

/* ATA Primary Bus registers */
#define ATA_DATA        0x1F0
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_CMD         0x1F7
#define ATA_STATUS      0x1F7
#define ATA_ALT_STATUS  0x3F6

/* ATA Secondary Bus registers */
#define ATA2_DATA       0x170
#define ATA2_SECCOUNT   0x172
#define ATA2_LBA_LO     0x173
#define ATA2_LBA_MID    0x174
#define ATA2_LBA_HI     0x175
#define ATA2_DRIVE      0x176
#define ATA2_CMD        0x177
#define ATA2_STATUS     0x177
#define ATA2_ALT_STATUS 0x376

/* Status bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Timeout in iterations (roughly 300ms worth of polls) */
#define ATA_TIMEOUT 100000

/* Returns 0 on success, -1 on timeout/error */
static int ata_poll_ready(uint16_t status_port) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(status_port);
        if (st == 0xFF) return -1;          /* No drive (floating bus) */
        if (st & ATA_SR_ERR) return -1;     /* Drive error */
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0; /* Ready! */
    }
    return -1; /* Timeout */
}

static inline uint16_t drive_port_reg(uint16_t data_port) {
    return data_port == ATA_DATA ? ATA_DRIVE : ATA2_DRIVE;
}

/* Detect a real ATA (non-ATAPI) drive at a position. The reliable test is
 * to issue IDENTIFY DEVICE (0xEC): ATA disks serve 512 bytes, while ATAPI
 * devices (CD-ROM) and empty slots abort the command. */
static int ata_drive_is_ata(uint16_t data_port, uint16_t status_port, uint8_t drive_sel) {
    uint16_t sec_port = data_port + 2;   /* ATA sector count reg */
    uint16_t cmd_port = data_port + 7;
    outb(drive_port_reg(data_port), drive_sel);
    for (int i = 0; i < 4; i++) inb(status_port);
    uint8_t st = inb(status_port);
    if (st == 0xFF || st == 0x00) return 0;

    outb(sec_port, 0);            /* sector count 0 (per spec) */
    outb(data_port + 3, 0);       /* LBA low */
    outb(data_port + 4, 0);       /* LBA mid */
    outb(data_port + 5, 0);       /* LBA high */
    outb(cmd_port, 0xEC);         /* IDENTIFY DEVICE */
    for (int i = 0; i < 4; i++) inb(status_port);
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        st = inb(status_port);
        if (st == 0xFF || (st & ATA_SR_ERR)) return 0;  /* ATAPI / no drive */
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) break;
        if (i == ATA_TIMEOUT - 1) return 0;
    }
    for (int i = 0; i < 256; i++) inw(data_port);   /* drain IDENTIFY data */
    return 1;
}

/* Detect which IDE bus/drive has our data disk. Order of preference:
 * primary slave -> secondary master -> secondary slave -> primary master.
 * ATAPI (CD-ROM) devices are skipped via signature check, so the probe is
 * safe regardless of where QEMU/hardware places the CD. */
static uint8_t disk_found = 0;
static uint16_t disk_base = 0;
static uint8_t disk_drive = 0;

/* Raw PIO primitives — bypass the block cache (used to fill/flush it) */
static int ata_read_sector_raw(uint32_t lba, uint8_t *buf);
static int ata_write_sector_raw(uint32_t lba, const uint8_t *buf);

void ata_detect(void) {
    static const struct {
        uint16_t data;
        uint16_t status;
        uint8_t  sel;
    } positions[4] = {
        { ATA_DATA,  ATA_STATUS,  0xB0 }, /* primary slave   */
        { ATA2_DATA, ATA2_STATUS, 0xE0 }, /* secondary master */
        { ATA2_DATA, ATA2_STATUS, 0xF0 }, /* secondary slave  */
        { ATA_DATA,  ATA_STATUS,  0xE0 }, /* primary master   */
    };
    for (int i = 0; i < 4; i++) {
        if (ata_drive_is_ata(positions[i].data, positions[i].status, positions[i].sel)) {
            disk_base  = positions[i].data;
            disk_drive = positions[i].sel;
            disk_found = 1;
            break;
        }
    }
    if (disk_found) {
        /* Write-back cache sits between the filesystem and this driver */
        bcache_init();
        bcache_register_driver(ata_read_sector_raw, ata_write_sector_raw);

        /* The very first read of the session can be served misaligned by the
         * controller; discard it and invalidate any cached copy so real
         * traffic always starts from a clean sector read. */
        uint8_t dummy[512];
        ata_read_sector_raw(0, dummy);
        bcache_invalidate_lba(0);
    }
}

/* ---- Raw PIO primitives (used by the block cache) ----------------------- */

static int ata_read_sector_raw(uint32_t lba, uint8_t *buf) {
    if (!disk_found) return -1;

    uint16_t status_port = (disk_base == ATA_DATA) ? ATA_STATUS  : ATA2_STATUS;
    uint16_t cmd_port    = (disk_base == ATA_DATA) ? ATA_CMD     : ATA2_CMD;
    uint16_t sec_port    = (disk_base == ATA_DATA) ? ATA_SECCOUNT : ATA2_SECCOUNT;
    uint16_t lo_port     = (disk_base == ATA_DATA) ? ATA_LBA_LO  : ATA2_LBA_LO;
    uint16_t mid_port    = (disk_base == ATA_DATA) ? ATA_LBA_MID : ATA2_LBA_MID;
    uint16_t hi_port     = (disk_base == ATA_DATA) ? ATA_LBA_HI  : ATA2_LBA_HI;
    uint16_t drv_port    = (disk_base == ATA_DATA) ? ATA_DRIVE   : ATA2_DRIVE;

    /* Wait for controller idle: BSY clear AND no stale DRQ left over from
     * the previous transfer (QEMU serves stale buffer data otherwise). */
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(status_port) & (ATA_SR_BSY | ATA_SR_DRQ))) break;
        if (i == ATA_TIMEOUT - 1) return -1;
    }

    /* Select drive + top LBA bits */
    outb(drv_port,  disk_drive | ((lba >> 24) & 0x0F));
    outb(sec_port,  1);
    outb(lo_port,   (uint8_t)lba);
    outb(mid_port,  (uint8_t)(lba >> 8));
    outb(hi_port,   (uint8_t)(lba >> 16));
    outb(cmd_port,  0x20); /* READ SECTORS */

    /* Wait for DRQ */
    if (ata_poll_ready(status_port) != 0) return -1;

    /* Read 256 16-bit words = 512 bytes */
    uint16_t *wbuf = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        wbuf[i] = inw(disk_base);
    }

    /* Wait for end-of-transfer (drive raises BSY briefly, then idle) */
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(status_port) & (ATA_SR_BSY | ATA_SR_DRQ))) break;
        if (i == ATA_TIMEOUT - 1) return -1;
    }
    return 0;
}

/* Public cached I/O — all reads/writes go through the write-back cache */
int ata_read_sector(uint32_t lba, uint8_t *buf) {
    return bcache_read(lba, buf);
}

/* Write one 512-byte sector to the detected IDE disk (LBA28 PIO).
 * Returns 0 on success, -1 on error/timeout/no-disk. */
static int ata_write_sector_raw(uint32_t lba, const uint8_t *buf) {
    if (!disk_found) return -1;

    uint16_t status_port = (disk_base == ATA_DATA) ? ATA_STATUS  : ATA2_STATUS;
    uint16_t cmd_port    = (disk_base == ATA_DATA) ? ATA_CMD     : ATA2_CMD;
    uint16_t sec_port    = (disk_base == ATA_DATA) ? ATA_SECCOUNT : ATA2_SECCOUNT;
    uint16_t lo_port     = (disk_base == ATA_DATA) ? ATA_LBA_LO  : ATA2_LBA_LO;
    uint16_t mid_port    = (disk_base == ATA_DATA) ? ATA_LBA_MID : ATA2_LBA_MID;
    uint16_t hi_port     = (disk_base == ATA_DATA) ? ATA_LBA_HI  : ATA2_LBA_HI;
    uint16_t drv_port    = (disk_base == ATA_DATA) ? ATA_DRIVE   : ATA2_DRIVE;

    /* Wait for controller ready: BSY clear and no stale DRQ */
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(status_port) & (ATA_SR_BSY | ATA_SR_DRQ))) break;
        if (i == ATA_TIMEOUT - 1) return -1;
    }

    /* Select drive + top LBA bits */
    outb(drv_port,  disk_drive | ((lba >> 24) & 0x0F));
    outb(sec_port,  1);
    outb(lo_port,   (uint8_t)lba);
    outb(mid_port,  (uint8_t)(lba >> 8));
    outb(hi_port,   (uint8_t)(lba >> 16));
    outb(cmd_port,  0x30); /* WRITE SECTORS */

    /* Wait for DRQ */
    if (ata_poll_ready(status_port) != 0) return -1;

    /* Write 256 16-bit words = 512 bytes */
    const uint16_t *wbuf = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        outw(disk_base, wbuf[i]);
    }

    /* Flush cache - wait for command completion */
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(status_port);
        if (st & ATA_SR_ERR) return -1;
        if (!(st & (ATA_SR_BSY | ATA_SR_DRQ))) return 0;
    }
    return -1;
}

/* Public cached write — marks the page dirty, disk I/O happens on flush */
int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    return bcache_write(lba, buf);
}

/* Write all dirty cached pages back to disk and issue ATA FLUSH CACHE.
 * Returns 0 on success, -1 if the disk is absent or a write failed. */
int ata_sync(void) {
    if (!disk_found) return -1;

    int result = bcache_flush();
    if (result != 0) return -1;

    uint16_t status_port = (disk_base == ATA_DATA) ? ATA_STATUS : ATA2_STATUS;
    uint16_t cmd_port    = (disk_base == ATA_DATA) ? ATA_CMD    : ATA2_CMD;
    uint16_t drv_port    = (disk_base == ATA_DATA) ? ATA_DRIVE  : ATA2_DRIVE;

    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(status_port) & (ATA_SR_BSY | ATA_SR_DRQ))) break;
        if (i == ATA_TIMEOUT - 1) return -1;
    }

    outb(drv_port, disk_drive);
    outb(cmd_port, 0xE7); /* FLUSH CACHE */

    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(status_port);
        if (st & ATA_SR_ERR) return -1;
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -1;
}

uint32_t ata_dirty_count(void) {
    return bcache_dirty_count();
}
