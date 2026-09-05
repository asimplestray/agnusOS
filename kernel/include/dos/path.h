#ifndef DOS_PATH_H
#define DOS_PATH_H

#include <stdint.h>

/*
 * AmigaDOS-style path parsing.
 *
 * Classic Amiga paths: "Volume:dir1/dir2/file.ext"
 * The colon separates the volume from the path.
 * No leading slash — "Work:docs/file.txt" not "/Work/docs/file.txt"
 */

/* Get the file part of a path (after last / or :) */
char *dos_file_part(const char *path);

/* Get the directory part of a path (before last / or :) */
char *dos_path_part(const char *path);

/* Join directory + filename: "Work:docs" + "file.txt" → "Work:docs/file.txt" */
int32_t dos_add_part(const char *dir, const char *file, char *buf, uint32_t bufsize);

/* Check if a path is absolute (contains :) */
int32_t dos_is_absolute(const char *path);

/* Parse AmigaOS wildcard pattern into internal form.
 * Returns 0 on success, -1 if pattern too complex. */
int32_t dos_parse_pattern(const char *pat, char *buf, uint32_t bufsize);

/* Match string against parsed pattern. Returns 1 if match, 0 if not. */
int32_t dos_match_pattern(const char *pat, const char *str);

#endif
