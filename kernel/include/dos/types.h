#ifndef DOS_TYPES_H
#define DOS_TYPES_H

#include <stdint.h>

/* File handle type (opaque, index into handle table) */
typedef uint32_t BPTR;

/* File open modes (AmigaDOS) */
#define MODE_OLDFILE      1005
#define MODE_NEWFILE      1006
#define MODE_READWRITE    1004
#define MODE_APPEND       1003

/* Lock types */
#define ACCESS_READ       (-2)
#define ACCESS_WRITE      (-1)

/* Seek origins */
#define OFFSET_BEGINNING  (-1)
#define OFFSET_CURRENT    0
#define OFFSET_END        1

/* DOS error codes — canonical definitions in <syscall.h> */
/* AOS_ERR_* codes are defined there to avoid duplication. */
/* DOS-specific codes not in syscall.h: */
#define AOS_ERR_BAD_NUMBER     20
#define AOS_ERR_LINE_TOO_LONG  30
#define AOS_ERR_NO_CONFIG      40
#define AOS_ERR_READ_ERROR     70
#define AOS_ERR_WRITE_ERROR    80
#define AOS_ERR_DIR_NOT_EMPTY 101
#define AOS_ERR_CANT_CHANGE   103

/* Protection bits (AmigaDOS fib_Protection) */
#define AOS_FIBF_READ    (1 << 0)
#define AOS_FIBF_WRITE   (1 << 1)
#define AOS_FIBF_EXECUTE (1 << 2)
#define AOS_FIBF_DELETE  (1 << 3)
#define AOS_FIBF_ARCHIVE (1 << 4)
#define AOS_FIBF_SCRIPT  (1 << 5)
#define AOS_FIBF_HIDDEN  (1 << 6)
#define AOS_FIBF_PURE    (1 << 7)

#endif
