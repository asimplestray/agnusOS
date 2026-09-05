#ifndef EXEC_LIBRARY_H
#define EXEC_LIBRARY_H

#include <stdint.h>

#define LIBRARY_NAME_MAX 32

typedef struct library {
    char        lib_Name[LIBRARY_NAME_MAX];
    uint32_t    lib_OpenCnt;
    uint32_t    lib_Version;
    uint32_t    lib_Revision;
    uint32_t    lib_Flags;
    const char *lib_IdString;
    void      (*lib_Init)(void);
    void      (*lib_Expunge)(void);
    struct library *lib_Next;
} library_t;

/* Open a library by name. Increments open count. */
library_t *exec_open_library(const char *name, uint32_t version);

/* Close a library. Decrements open count, calls Expunge if 0. */
void exec_close_library(library_t *lib);

/* Find a library by name (does not increment open count). */
library_t *exec_find_library(const char *name);

/* Add a library to the global list. */
void exec_add_library(library_t *lib);

#endif
