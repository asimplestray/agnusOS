/*
 * compat/linux_spinlock.h — spinlocks estilo Linux sobre os nativos.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * Mapeamento 1:1 (ver GPU_PORTING_STRATEGY.md seção 3):
 *   spinlock_t       -> spinlock_t nativo          (<spinlock.h>)
 *   spinlock_irqsave -> spinlock_irq_t nativo      (<spinlock.h>)
 *
 * Os nomes spin_lock/spin_unlock/spin_trylock já existem no nativo com a
 * mesma semântica. Este header adiciona apenas os nomes do Linux que
 * faltam, via macros/inline com tokens novos (sem colisão).
 */

#ifndef _COMPAT_LINUX_SPINLOCK_H_
#define _COMPAT_LINUX_SPINLOCK_H_

#include <spinlock.h>

/* Linux: spin_lock_init(l)  -> nativo: spinlock_init(l) */
#define spin_lock_init(lock) spinlock_init(lock)

/* DEFINE_SPINLOCK estilo Linux */
#define DEFINE_SPINLOCK(name) spinlock_t name = SPINLOCK_INIT

/* Variante irqsave que devolve flags (assinatura Linux clássica).
 * Usa token próprio para não colidir com o par nativo. */
static inline unsigned long spin_lock_irqsave_compat(spinlock_t *lock)
{
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore_compat(spinlock_t *lock,
                                                 unsigned long flags)
{
    spin_unlock(lock);
    if (flags & (1UL << 9))
        __asm__ volatile("sti");
}

static inline int spin_is_locked(spinlock_t *lock)
{
    return lock->locked != 0;
}

#endif /* _COMPAT_LINUX_SPINLOCK_H_ */
