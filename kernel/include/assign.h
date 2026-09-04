#ifndef ASSIGN_H
#define ASSIGN_H

#include <stdint.h>

/* Logical volume names (AmigaOS-style assigns).
 *
 * An assign maps a short name (e.g. "Sys", "Work", "C") to a directory
 * path.  Any path of the form "Name:rest" is transparently expanded to
 * "<assigned-path>/rest" by vfs_resolve(), so userspace can address
 * volumes like Sys:bin/hello.elf or Work:docs/readme.
 */

#define ASSIGN_MAX_NAME  32
#define ASSIGN_MAX_PATH  256

/* op codes for the assign syscall */
#define ASSIGN_SET    0
#define ASSIGN_UNSET  1
#define ASSIGN_GET    2

/* Assign a name to a path (path must be absolute). Returns 0 on success,
 * -1 on error.  An existing assignment is overwritten in place. */
int  assign_set(const char *name, const char *path);

/* Remove an assignment. Returns 0 on success, -1 if not found. */
int  assign_unset(const char *name);

/* Copy the path bound to name into out. Returns 0 if found, -1 if not. */
int  assign_lookup(const char *name, char *out, int out_size);

/* If path starts with "Name:" expand it to the assigned absolute path in
 * out.  Returns 0 when expanded, -1 when path is not an assign reference. */
int  assign_expand(const char *path, char *out, int out_size);

/* Register the default system assigns (Sys:, Ram:, Work:, C:, Devs:). */
void assign_init(void);

/* Dump "name: -> path" lines into buf (used by /proc/assigns). */
int  assign_dump(char *buf, int max);

/* Boot-time selftest exercised through vfs_resolve(). */
void assign_test(void);

#endif