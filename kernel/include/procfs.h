#ifndef PROCFS_H
#define PROCFS_H

#include <vfs.h>

/* Build and mount a dynamic procfs at /proc on top of RamFS:
 *   /proc/meminfo     — RAM totals and dirty disk sectors
 *   /proc/uptime      — seconds since boot
 *   /proc/<pid>/status — per-process status
 */
void procfs_init(void);

#endif