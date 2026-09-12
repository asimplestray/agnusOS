/*
 * ELF64 Loader — loads a static ELF64 executable into a specific process's PML4.
 *
 * Only PT_LOAD segments are processed. BSS (.p_memsz > p_filesz) is
 * zeroed automatically. Each 4 KB page is allocated via pmm_alloc_block()
 * and mapped with vmm_map_page_in_pml4() using the segment's flags.
 *
 * Returns the ELF entry point on success, 0 on any validation error.
 */

#include <elf.h>
#include <vmm.h>
#include <pmm.h>
#include <screen.h>
#include <task.h>
#include <stdint.h>
#include <stddef.h>

static void mem_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void mem_zero(void *dst, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

/*
 * Map a contiguous virtual region [vaddr, vaddr+size) in the target PML4
 * backed by freshly allocated physical pages. Returns the kernel-visible
 * (identity-mapped) address of the first byte, or 0 on OOM.
 */
static uint64_t map_region(uint64_t pml4_phys, uint64_t vaddr, uint64_t size, uint64_t flags)
{
    uint64_t aligned_start = vaddr & ~(PAGE_SIZE - 1);
    uint64_t aligned_end   = (vaddr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t first_phys    = 0;

    for (uint64_t va = aligned_start; va < aligned_end; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_block();
        if (!phys) {
            screen_log("FAIL", COLOR_LIGHT_RED, "ELF: pmm OOM ao mapear segmento");
            return 0;
        }
        if (!first_phys && va == aligned_start)
            first_phys = phys;

        /* Zero the freshly allocated page */
        mem_zero((void *)phys, PAGE_SIZE);

        if (!vmm_map_page_in_pml4(pml4_phys, va, phys, flags)) {
            pmm_free_block(phys);
            return 0;
        }
    }

    /* Physical address of vaddr (identity-mapped kernel, offset within page) */
    uint64_t first_page_phys = first_phys ? first_phys : 0;
    uint64_t page_offset = vaddr - aligned_start;
    return first_page_phys + page_offset;
}

uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, uint32_t size)
{
    return elf_load_mm(pml4_phys, data, size, NULL);
}

uint64_t elf_load_mm(uint64_t pml4_phys, const uint8_t *data, uint32_t size,
                     struct mm_struct *mm)
{
    if (!data || size < sizeof(Elf64_Ehdr)) {
        screen_log("FAIL", COLOR_LIGHT_RED, "ELF: buffer muito pequeno");
        return 0;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* Validate magic */
    if (ehdr->e_ident[0] != ELF_MAGIC0 ||
        ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 ||
        ehdr->e_ident[3] != ELF_MAGIC3)
    {
        screen_log("FAIL", COLOR_LIGHT_RED, "ELF: magic invalido");
        return 0;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        screen_log("FAIL", COLOR_LIGHT_RED, "ELF: nao e ELF64");
        return 0;
    }

    if (ehdr->e_machine != EM_X86_64) {
        screen_log("FAIL", COLOR_LIGHT_RED, "ELF: arquitetura nao e x86_64");
        return 0;
    }

    if (ehdr->e_type != ET_EXEC) {
        screen_log("FAIL", COLOR_LIGHT_RED, "ELF: nao e executavel (ET_EXEC)");
        return 0;
    }

    /* Iterate program headers */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        uint64_t ph_offset = ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize;
        if (ph_offset + sizeof(Elf64_Phdr) > size) break;

        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ph_offset);
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0)      continue;

        /* Determine mapping flags (W^X: data without exec gets NX). */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (phdr->p_flags & PF_W) flags |= VMM_FLAG_WRITE;
        if (!(phdr->p_flags & PF_X)) flags |= VMM_FLAG_NX;

        /* Map destination virtual region and get kernel-visible pointer */
        uint64_t dst_kern = map_region(pml4_phys, phdr->p_vaddr, phdr->p_memsz, flags);
        if (!dst_kern) return 0;

        if (mm) {
            uint64_t start = phdr->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t end = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            vma_add(mm, start, end, flags, VMA_TYPE_ELF);
        }

        /* Copy file image */
        if (phdr->p_filesz > 0) {
            if (phdr->p_offset + phdr->p_filesz > size) {
                screen_log("FAIL", COLOR_LIGHT_RED, "ELF: segmento fora dos limites do buffer");
                return 0;
            }
            mem_copy((void *)dst_kern, data + phdr->p_offset, phdr->p_filesz);
        }

        /* BSS: memory beyond filesz is already zeroed by map_region */
    }

    screen_log("OK", COLOR_LIGHT_GREEN, "ELF64 carregado com sucesso.");
    return ehdr->e_entry;
}
