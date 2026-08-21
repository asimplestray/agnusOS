/*
 * compat/linux_types.h — tipos básicos e utilitários estilo Linux.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * Convenção da compat layer (ver GPU_PORTING_STRATEGY.md seção 3):
 *   - Headers privados em kernel/include/compat/, wrappers finos 1:1 com as
 *     APIs nativas do ApolloOS.
 *   - Nada de alocadores Linux (kmalloc/vmalloc já são nativos), nada de
 *     vazamento de abstrações para fora de drivers em port.
 */

#ifndef _COMPAT_LINUX_TYPES_H_
#define _COMPAT_LINUX_TYPES_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Tipos de largura fixa estilo Linux */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;

typedef unsigned int  uint;
typedef unsigned long ulong;

/* container_of — igual ao Linux; nativo não fornece */
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef clamp
#define clamp(val, lo, hi) min(max(val, lo), hi)
#endif

#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((typeof(x))(a) - 1))
#endif

#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))

/* Barreiras de compilador (single-core: apenas compiler barrier) */
#define barrier() __asm__ volatile("" ::: "memory")
#define mb()      __asm__ volatile("mfence" ::: "memory")
#define rmb()     barrier()
#define wmb()     barrier()

#endif /* _COMPAT_LINUX_TYPES_H_ */
