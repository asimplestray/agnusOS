#ifndef DEVFS_H
#define DEVFS_H

#include <vfs.h>

/* Build and mount a devfs at /dev on top of RamFS:
 *   /dev/hda  — raw ATA block device (byte offset -> sector LBA)
 *   /dev/tty0 — console: writes to VGA text screen, reads from keyboard
 *   /dev/kbd  — keyboard character device
 * Opening a node routes the file operations to the underlying driver.
 */
void devfs_init(void);

#endif