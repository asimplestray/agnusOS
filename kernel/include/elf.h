#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

/* ELF magic */
#define ELF_MAGIC0  0x7F
#define ELF_MAGIC1  'E'
#define ELF_MAGIC2  'L'
#define ELF_MAGIC3  'F'

/* EI_CLASS */
#define ELFCLASS64  2

/* EI_DATA */
#define ELFDATA2LSB 1   /* Little-endian */

/* e_type */
#define ET_EXEC     2

/* e_machine */
#define EM_X86_64   62

/* Program header p_type */
#define PT_LOAD     1

/* Program header p_flags */
#define PF_X  0x1   /* Execute */
#define PF_W  0x2   /* Write   */
#define PF_R  0x4   /* Read    */

/* ELF64 file header */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;       /* Entry point virtual address */
    uint64_t e_phoff;       /* Program header table offset */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;       /* Number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* ELF64 program header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;   /* Offset in the file */
    uint64_t p_vaddr;    /* Virtual address to load at */
    uint64_t p_paddr;
    uint64_t p_filesz;   /* Bytes in file image */
    uint64_t p_memsz;    /* Bytes in memory (>= filesz; BSS padding) */
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/*
 * Validate and load an ELF64 static executable from memory into a specific PML4.
 *
 * Maps each PT_LOAD segment into the target process's address space.
 * When mm is non-NULL, each segment is also registered as a VMA so page
 * faults can be authorized against it. W^X: non-executable segments get NX.
 *
 * Returns the entry point virtual address on success, 0 on failure.
 */
struct mm_struct;
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, uint32_t size);
uint64_t elf_load_mm(uint64_t pml4_phys, const uint8_t *data, uint32_t size,
                     struct mm_struct *mm);

#endif
