#include <exec/exec.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>

/* ------------------------------------------------------------------ */
/* Library system                                                       */
/* ------------------------------------------------------------------ */

void exec_add_library(library_t *lib) {
    if (!lib || !SysBase) return;

    unsigned long flags;
    spin_lock_irqsave(&SysBase->eb_SchedLock, &flags);

    lib->lib_Next = SysBase->eb_LibHead;
    SysBase->eb_LibHead = lib;
    SysBase->eb_LibCount++;

    spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);
}

library_t *exec_find_library(const char *name) {
    if (!name || !SysBase) return NULL;

    unsigned long flags;
    spin_lock_irqsave(&SysBase->eb_SchedLock, &flags);

    library_t *lib = SysBase->eb_LibHead;
    while (lib) {
        if (strcmp(lib->lib_Name, name) == 0) {
            spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);
            return lib;
        }
        lib = lib->lib_Next;
    }

    spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);
    return NULL;
}

library_t *exec_open_library(const char *name, uint32_t version) {
    library_t *lib = exec_find_library(name);
    if (!lib) return NULL;

    if (lib->lib_Version < version) return NULL;

    lib->lib_OpenCnt++;

    serial_print("EXEC: OpenLibrary \"");
    serial_print(name);
    serial_print("\" v");
    char buf[16];
    itoa(version, buf, 10);
    serial_print(buf);
    serial_print(" open=");
    itoa(lib->lib_OpenCnt, buf, 10);
    serial_print(buf);
    serial_print("\n");

    return lib;
}

void exec_close_library(library_t *lib) {
    if (!lib) return;

    if (lib->lib_OpenCnt > 0) {
        lib->lib_OpenCnt--;

        serial_print("EXEC: CloseLibrary \"");
        serial_print(lib->lib_Name);
        serial_print("\" open=");
        char buf[16];
        itoa(lib->lib_OpenCnt, buf, 10);
        serial_print(buf);
        serial_print("\n");

        /* If last close, call Expunge */
        if (lib->lib_OpenCnt == 0 && lib->lib_Expunge) {
            lib->lib_Expunge();
        }
    }
}
