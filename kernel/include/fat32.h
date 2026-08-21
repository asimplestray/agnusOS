#ifndef FAT32_H
#define FAT32_H

#include <vfs.h>

/* Initialize the FAT32 driver on the primary master ATA disk,
 * and mount it onto the provided VFS node. */
int fat32_init_and_mount(vfs_node_t *mount_node);

/* Directory operations (VFAT long filenames supported) */
int fat32_create_file(vfs_node_t *dir_node, const char *name);
int fat32_create_dir(vfs_node_t *dir_node, const char *name);
int fat32_delete_entry(vfs_node_t *dir_node, const char *name);
vfs_dirent_t *fat32_readdir(vfs_node_t *node, uint32_t index);

#endif
