/*
 * compat/linux_mutex.h — mutex estilo Linux sobre o mutex_t nativo.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * Mapeamento 1:1 (ver GPU_PORTING_STRATEGY.md seção 3):
 *   struct mutex (Linux) -> mutex_t nativo (<spinlock.h>)
 *
 * CONVENÇÃO: o tipo nativo mutex_t É o `struct mutex` do Linux — os nomes
 * mutex_init/mutex_lock/mutex_unlock/mutex_trylock já existem como static
 * inline nativos com a mesma semântica e NÃO são redeclarados aqui
 * (colisão de símbolos). Este header fornece apenas aliases e helpers que
 * faltam no nativo. Código em port usa mutex_t diretamente.
 */

#ifndef _COMPAT_LINUX_MUTEX_H_
#define _COMPAT_LINUX_MUTEX_H_

#include <spinlock.h>
#include <stdbool.h>

/* Linux: DEFINE_MUTEX(name) cria e inicializa */
#define DEFINE_MUTEX(name) mutex_t name = MUTEX_INIT

/* Helper de init com nome único (não colide com mutex_init nativo) */
static inline void mutex_compat_init(mutex_t *m)
{
    mutex_init(m);
}

static inline bool mutex_is_locked(mutex_t *m)
{
    return m->count <= 0;
}

/* mutex_destroy não existe no nativo (mutex leve, sem waitqueue) */
static inline void mutex_compat_destroy(mutex_t *m)
{
    (void)m;
}

#endif /* _COMPAT_LINUX_MUTEX_H_ */
