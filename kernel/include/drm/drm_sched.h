/*
 * drm/drm_sched.h — DRM scheduler nativo (job queue / entity / runqueue /
 * timeout-recovery).
 *
 * Fase 2 Dev 2 — entrega: fila de 1000 jobs completa em ordem; job que
 * faz timeout é abortado com callback de recovery do driver.
 *
 * Modelo (subset do Linux drm_sched, adaptado ao workqueue nativo):
 *   - Cada entity tem uma FIFO própria; o scheduler serve as entities
 *     em round-robin para um único "ring" de hardware (1 job em
 *     execução por vez, como um GFX ring).
 *   - ops->run_job() submete ao hardware e devolve a finished fence.
 *   - A conclusão assíncrona reagenda o pump via workqueue; conclusão
 *     síncrona é detectada no próprio loop do pump.
 *   - Timeout: verificado no pump. Se o job exceder timeout_ticks sem a
 *     fence terminar, é abortado e ops->timedout_job() roda (recovery).
 *
 * Contexto de execução: pump roda no contexto de flush_workqueue()
 * (processo/ caller). timedout_job pode ser acionado nesse contexto —
 * drivers devem fazer recovery pesado via workqueue própria.
 */

#ifndef _DRM_SCHED_H_
#define _DRM_SCHED_H_

#include <stdint.h>
#include <stdbool.h>
#include <spinlock.h>
#include <workqueue.h>
#include <drm/dma_fence.h>

#define DRM_SCHED_MAX_ENTITIES 8
#define DRM_SCHED_NAME_LEN     24

/* Estados do job */
enum drm_sched_job_state {
    DRM_SCHED_JOB_PENDING = 0,
    DRM_SCHED_JOB_RUNNING,
    DRM_SCHED_JOB_DONE,
    DRM_SCHED_JOB_ABORTED,
};

struct drm_gpu_scheduler;

/* Entity: stream de submissão FIFO (um processo/contexto GL, p.ex.) */
struct drm_sched_entity {
    struct drm_gpu_scheduler *sched;
    struct drm_sched_job     *head;
    struct drm_sched_job     *tail;
    spinlock_irq_t            lock;
    int                       id;
    bool                      fini;       /* não aceita mais jobs */
    uint64_t                  queued;     /* contagem já enfileirada */
};

/* Job unitário de trabalho */
struct drm_sched_job {
    /* FIFO node dentro da entity */
    struct drm_sched_job      *next;

    struct drm_sched_entity   *entity;
    void                      *priv;      /* payload do driver */

    /* Fence do job na timeline do scheduler (contexto único por sched) */
    struct dma_fence          *fence;
    struct dma_fence_cb        finish_cb; /* ligado à hw fence */

    struct dma_fence          *hw_fence;  /* retornado por run_job (owned) */
    uint64_t                   start_tick;
    enum drm_sched_job_state   state;
};

/* Ops do driver dondo hardware */
struct drm_sched_ops {
    /*
     * Submete o job ao hardware. Deve retornar rápido (apenas push em
     * ring/wq). Retorna a fence que sinaliza a conclusão (nunca NULL).
     */
    struct dma_fence *(*run_job)(struct drm_sched_job *job);

    /*
     * Job estourou timeout: aborta/recovery. Chamado com o sched lock
     * SOLTO. O job NÃO será reexecutado; após retorno o scheduler
     * segue para o próximo job.
     */
    void (*timedout_job)(struct drm_sched_job *job);

    /* Opcional: libera recursos próprios do job (não o job em si). */
    void (*free_job)(struct drm_sched_job *job);
};

struct drm_gpu_scheduler {
    char                       name[DRM_SCHED_NAME_LEN];
    const struct drm_sched_ops *ops;
    uint32_t                    timeout_ticks;
    uint64_t                    context;      /* timeline dma_fence */
    uint64_t                    job_seqno;    /* seqno incremental do job */

    struct workqueue_struct    *wq;
    struct work_struct          work;         /* pump */

    spinlock_irq_t              lock;
    struct drm_sched_entity    *entities[DRM_SCHED_MAX_ENTITIES];
    int                         num_entities;
    int                         rr_index;     /* round-robin */

    struct drm_sched_job       *running;
    bool                        in_pump;
    bool                        stopped;
};

/* ---- Scheduler ---- */
int  drm_sched_init(struct drm_gpu_scheduler *sched,
                    const struct drm_sched_ops *ops,
                    const char *name,
                    uint32_t timeout_ticks);
void drm_sched_fini(struct drm_gpu_scheduler *sched);

/* Drena o workqueue do scheduler (roda pump até esvaziar). */
void drm_sched_flush(struct drm_gpu_scheduler *sched);

/* ---- Entity ---- */
int  drm_sched_entity_init(struct drm_sched_entity *entity,
                           struct drm_gpu_scheduler *sched);
void drm_sched_entity_fini(struct drm_sched_entity *entity);

/* ---- Jobs ---- */
int  drm_sched_job_init(struct drm_sched_job *job,
                        struct drm_sched_entity *entity,
                        void *priv);
void drm_sched_entity_push_job(struct drm_sched_job *job,
                               struct drm_sched_entity *entity);
struct dma_fence *drm_sched_job_fence(struct drm_sched_job *job);

/* ---- Selftest de boot ---- */
int drm_sched_test(void);

#endif /* _DRM_SCHED_H_ */
