/*
 * compat/linux_work.h — workqueue estilo Linux sobre a nativa.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * Mapeamento 1:1 (ver GPU_PORTING_STRATEGY.md seção 3):
 *   struct work_struct      -> work_struct nativo (<workqueue.h>)
 *   INIT_WORK/INIT_DELAYED  -> macros nativas de mesmo nome
 *   queue_work/flush        -> funções nativas de mesmo nome
 *
 * Este header adiciona apenas os nomes do Linux ausentes no nativo.
 */

#ifndef _COMPAT_LINUX_WORK_H_
#define _COMPAT_LINUX_WORK_H_

#include <workqueue.h>
#include <compat/linux_types.h>

/* Linux: schedule_work(w) == queue_work(system_wq, w) */
static inline int schedule_work(struct work_struct *work)
{
    extern struct workqueue_struct *system_wq;
    return queue_work(system_wq, work);
}

static inline int schedule_delayed_work(struct work_struct *work,
                                        uint64_t delay_ticks)
{
    extern struct workqueue_struct *system_wq;
    return queue_delayed_work(system_wq, work, delay_ticks);
}

/* Linux: flush_scheduled_work() drena o system_wq */
static inline void flush_scheduled_work(void)
{
    extern struct workqueue_struct *system_wq;
    flush_workqueue(system_wq);
}

#endif /* _COMPAT_LINUX_WORK_H_ */
