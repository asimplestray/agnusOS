#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>

typedef uint32_t BPTR;
typedef uint32_t APTR;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int16_t  WORD;
typedef uint16_t UWORD;
typedef int8_t   BYTE;
typedef uint8_t  UBYTE;
typedef int32_t  BOOL;

#define TRUE  1
#define FALSE 0
#define NULL_BPTR 0

#define EXEC_VERSION    1
#define EXEC_REVISION  0
#define EXEC_MAGIC     0x414F5300

/* Memory flags */
#define MEMF_PUBLIC   (1UL << 0)
#define MEMF_CHIP     (1UL << 1)
#define MEMF_FAST     (1UL << 2)
#define MEMF_CLEAR    (1UL << 16)
#define MEMF_REVERSE  (1UL << 17)
#define MEMF_LARGEST  (1UL << 18)

#endif
