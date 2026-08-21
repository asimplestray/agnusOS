/*
 * drm_sched.c — DRM scheduler nativo.
 *
 * Fase 2 Dev 2. Ver drm/drm_sched.h para o modelo de execução.
 */

#include <drm/drm_sched.h>
#include <compat/linux_types.h>
#include <compat/linux_fence.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>
#include <timer.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

static void sched_log(const char *tag, vga_color_t color, const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_print("[DRM-SCHED] ");
    serial_print(buf);
    serial_print("\n");
    screen_log(tag, color, buf);
}

/* ------------------------------------------------------------------ */
/* Job lifecycle                                                       */
/* ------------------------------------------------------------------ */

int drm_sched_job_init(struct drm_sched_job *job,
                       struct drm_sched_entity *entity,
                       void *priv)
{
    struct drm_gpu_scheduler *sched;
    spinlock_irq_t lock_tmp;

    if (!job || !entity || !entity->sched)
        return -EINVAL;
    sched = entity->sched;

    memset(job, 0, sizeof(*job));
    job->entity = entity;
    job->priv = priv;
    job->state = DRM_SCHED_JOB_PENDING;

    /* Fence própria na timeline do scheduler (alocada separadamente:
     * o release default da dma_fence faz kfree do ponteiro). */
    memset(&lock_tmp, 0, sizeof(lock_tmp));

    job->fence = kmalloc(sizeof(struct dma_fence));
    if (!job->fence)
        return -ENOMEM;
    dma_fence_init(job->fence, &dma_fence_default_ops, &lock_tmp,
                   sched->context,
                   __sync_fetch_and_add(&sched->job_seqno, 1));
    return 0;
}

struct dma_fence *drm_sched_job_fence(struct drm_sched_job *job)
{
    return job ? job->fence : NULL;
}

/* Chamado com sched->lock segurado OU job já fora das filas. */
static void sched_job_cleanup(struct drm_sched_job *job)
{
    if (job->hw_fence) {
        dma_fence_put(job->hw_fence);
        job->hw_fence = NULL;
    }
    if (job->fence) {
        dma_fence_put(job->fence);
        job->fence = NULL;
    }
    if (job->entity && job->entity->sched &&
        job->entity->sched->ops && job->entity->sched->ops->free_job)
        job->entity->sched->ops->free_job(job);
}

/* ------------------------------------------------------------------ */
/* Pump                                                                */
/* ------------------------------------------------------------------ */

/* Pega o próximo job (round-robin entre entities). sched->lock held. */
static struct drm_sched_job *sched_pop_next_locked(
    struct drm_gpu_scheduler *sched)
{
    for (int tried = 0; tried < sched->num_entities; tried++) {
        int idx = (sched->rr_index + tried) % sched->num_entities;
        struct drm_sched_entity *e = sched->entities[idx];
        unsigned long eflags;

        spin_lock_irqsave(&e->lock, &eflags);
        struct drm_sched_job *job = e->head;
        if (job) {
            e->head = job->next;
            if (!e->head)
                e->tail = NULL;
            spin_unlock_irqrestore(&e->lock, eflags);
            sched->rr_index = (idx + 1) % sched->num_entities;
            return job;
        }
        spin_unlock_irqrestore(&e->lock, eflags);
    }
    return NULL;
}

/* done_cb: hardware terminou o job corrente */
static void sched_job_done_cb(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct drm_sched_job *job =
        container_of(cb, struct drm_sched_job, finish_cb);
    struct drm_gpu_scheduler *sched = job->entity->sched;

    (void)f;
    job->state = DRM_SCHED_JOB_DONE;

    /* Se o pump estiver rodando ele mesmo detecta a conclusão no loop;
     * caso contrário reagenda o pump para colher o resultado. */
    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    bool pumping = sched->in_pump;
    spin_unlock_irqrestore(&sched->lock, flags);

    if (!pumping)
        queue_work(sched->wq, &sched->work);
}

static void sched_start_running_locked(struct drm_gpu_scheduler *sched,
                                       struct drm_sched_job *job)
{
    job->state = DRM_SCHED_JOB_RUNNING;
    job->start_tick = timer_get_ticks();
    sched->running = job;
}

/*
 * Uma passada do pump. Retorna true se houve progresso (deve repetir),
 * false se não há nada a fazer agora.
 */
static bool sched_pump_once(struct drm_gpu_scheduler *sched)
{
    struct drm_sched_job *job;

    /* 1) Job em execução: concluído? timeout? */
    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    job = sched->running;
    if (job) {
        bool done = job->hw_fence && dma_fence_is_signaled(job->hw_fence);
        bool timed_out = !done && sched->timeout_ticks > 0 &&
                         (timer_get_ticks() - job->start_tick) >=
                             sched->timeout_ticks;

        if (done) {
            sched->running = NULL;
            spin_unlock_irqrestore(&sched->lock, flags);
            sched_job_cleanup(job);   /* solta fences (+ free_job op) */
            return true;
        }

        if (timed_out) {
            sched->running = NULL;
            job->state = DRM_SCHED_JOB_ABORTED;
            spin_unlock_irqrestore(&sched->lock, flags);

            sched_log("WARN", COLOR_BROWN,
                      "job timeout em '%s' — abortando (recovery)",
                      sched->name);
            dma_fence_signal_error(job->fence, -ETIMEDOUT);
            if (sched->ops->timedout_job)
                sched->ops->timedout_job(job);
            sched_job_cleanup(job);
            return true;
        }

        /* ainda executando dentro do prazo */
        spin_unlock_irqrestore(&sched->lock, flags);
        return false;
    }
    spin_unlock_irqrestore(&sched->lock, flags);

    /* 2) Nada rodando: busca próximo job */
    spin_lock_irqsave(&sched->lock, &flags);
    job = sched_pop_next_locked(sched);
    if (!job) {
        spin_unlock_irqrestore(&sched->lock, flags);
        return false;
    }

    if (sched->stopped) {
        spin_unlock_irqrestore(&sched->lock, flags);
        job->state = DRM_SCHED_JOB_ABORTED;
        dma_fence_signal_error(job->fence, -EINVAL);
        sched_job_cleanup(job);
        return true;
    }

    sched_start_running_locked(sched, job);
    spin_unlock_irqrestore(&sched->lock, flags);

    /* run_job FORA do lock (pode dormir/enfileirar trabalho) */
    struct dma_fence *hw = sched->ops->run_job(job);
    if (!hw) {
        /* driver quebrou contrato: aborta job */
        sched_log("ERROR", COLOR_RED, "run_job retornou NULL — abortando");
        unsigned long f2;
        spin_lock_irqsave(&sched->lock, &f2);
        if (sched->running == job)
            sched->running = NULL;
        spin_unlock_irqrestore(&sched->lock, f2);
        job->state = DRM_SCHED_JOB_ABORTED;
        dma_fence_signal_error(job->fence, -EINVAL);
        sched_job_cleanup(job);
        return true;
    }

    job->hw_fence = hw;   /* referência owned do scheduler */

    spin_lock_irqsave(&sched->lock, &flags);
    bool already_done = dma_fence_is_signaled(hw);
    if (already_done) {
        if (sched->running == job)
            sched->running = NULL;
        spin_unlock_irqrestore(&sched->lock, flags);
        job->state = DRM_SCHED_JOB_DONE;
        dma_fence_signal(job->fence);
        sched_job_cleanup(job);
        return true;
    }
    spin_unlock_irqrestore(&sched->lock, flags);

    dma_fence_add_callback(hw, &job->finish_cb, sched_job_done_cb);
    return true;
}

static void sched_pump_work(struct work_struct *w)
{
    struct drm_gpu_scheduler *sched =
        container_of(w, struct drm_gpu_scheduler, work);
    bool progress;

    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    if (sched->in_pump) {
        spin_unlock_irqrestore(&sched->lock, flags);
        return;
    }
    sched->in_pump = true;
    spin_unlock_irqrestore(&sched->lock, flags);

    do {
        progress = sched_pump_once(sched);
    } while (progress);

    spin_lock_irqsave(&sched->lock, &flags);
    sched->in_pump = false;
    spin_unlock_irqrestore(&sched->lock, flags);
}

/* ------------------------------------------------------------------ */
/* Scheduler / entity lifecycle                                        */
/* ------------------------------------------------------------------ */

int drm_sched_init(struct drm_gpu_scheduler *sched,
                   const struct drm_sched_ops *ops,
                   const char *name,
                   uint32_t timeout_ticks)
{
    if (!sched || !ops || !ops->run_job || !name)
        return -EINVAL;

    memset(sched, 0, sizeof(*sched));
    strncpy(sched->name, name, sizeof(sched->name) - 1);
    sched->ops = ops;
    sched->timeout_ticks = timeout_ticks;
    sched->context = dma_fence_context_alloc(1);
    spinlock_init(&sched->lock.lock);

    sched->wq = alloc_workqueue(name, 0);
    if (!sched->wq)
        return -ENOMEM;
    INIT_WORK(&sched->work, sched_pump_work);

    sched_log("INFO", COLOR_LIGHT_CYAN,
              "scheduler '%s' pronto (timeout=%u ticks)",
              name, timeout_ticks);
    return 0;
}

void drm_sched_fini(struct drm_gpu_scheduler *sched)
{
    if (!sched || !sched->wq)
        return;

    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    sched->stopped = true;
    spin_unlock_irqrestore(&sched->lock, flags);

    flush_workqueue(sched->wq);
    destroy_workqueue(sched->wq);
    sched->wq = NULL;
    sched_log("INFO", COLOR_LIGHT_CYAN, "scheduler '%s' destruído",
              sched->name);
}

void drm_sched_flush(struct drm_gpu_scheduler *sched)
{
    if (!sched || !sched->wq)
        return;
    queue_work(sched->wq, &sched->work);
    flush_workqueue(sched->wq);
}

int drm_sched_entity_init(struct drm_sched_entity *entity,
                          struct drm_gpu_scheduler *sched)
{
    if (!entity || !sched)
        return -EINVAL;
    if (sched->num_entities >= DRM_SCHED_MAX_ENTITIES)
        return -ENOMEM;

    memset(entity, 0, sizeof(*entity));
    entity->sched = sched;
    entity->id = sched->num_entities;
    spinlock_init(&entity->lock.lock);

    unsigned long flags;
    spin_lock_irqsave(&sched->lock, &flags);
    sched->entities[sched->num_entities++] = entity;
    spin_unlock_irqrestore(&sched->lock, flags);

    return 0;
}

void drm_sched_entity_fini(struct drm_sched_entity *entity)
{
    unsigned long flags;

    if (!entity || !entity->sched)
        return;

    spin_lock_irqsave(&entity->lock, &flags);
    entity->fini = true;
    spin_unlock_irqrestore(&entity->lock, flags);
}

void drm_sched_entity_push_job(struct drm_sched_job *job,
                               struct drm_sched_entity *entity)
{
    unsigned long flags;

    if (!job || !entity)
        return;

    spin_lock_irqsave(&entity->lock, &flags);
    job->next = NULL;
    if (entity->tail)
        entity->tail->next = job;
    else
        entity->head = job;
    entity->tail = job;
    entity->queued++;
    spin_unlock_irqrestore(&entity->lock, flags);

    /* Acorda o pump */
    if (entity->sched)
        queue_work(entity->sched->wq, &entity->sched->work);
}

/* ------------------------------------------------------------------ */
/* Selftest de boot                                                    */
/* ------------------------------------------------------------------ */

#define SCHED_TEST_JOBS 1000
#define SCHED_TEST_MAX_DONE 2048

struct sched_test_priv {
    int seq;      /* índice de submissão */
};

static struct drm_gpu_scheduler test_sched;
static struct drm_sched_entity  test_ent_a;
static struct drm_sched_entity  test_ent_b;

static volatile int  done_order[SCHED_TEST_MAX_DONE];
static volatile int  done_count;
static bool          make_stuck;          /* run_job devolve fence presa */
static struct dma_fence *stuck_hw_fence;  /* nunca sinalizada */
static volatile int  recovery_count;

/* hw fence alocada pelo "hardware" fake */
static struct dma_fence *test_hw_fence_alloc(uint64_t context)
{
    spinlock_irq_t tmp;
    struct dma_fence *hw = kmalloc(sizeof(struct dma_fence));
    if (!hw)
        return NULL;
    memset(&tmp, 0, sizeof(tmp));
    dma_fence_init(hw, &dma_fence_default_ops, &tmp, context,
                   __sync_fetch_and_add(&test_sched.job_seqno, 1));
    return hw;
}

static struct dma_fence *test_run_job(struct drm_sched_job *job)
{
    struct sched_test_priv *priv = job->priv;
    struct dma_fence *hw = test_hw_fence_alloc(test_sched.context + 1);

    if (!hw)
        return NULL;

    if (make_stuck && !stuck_hw_fence) {
        /* hardware travou: fence fica pendente */
        stuck_hw_fence = hw;
        return hw;
    }

    /* hardware completa sincronamente (ordem de execução == FIFO) */
    if (done_count < SCHED_TEST_MAX_DONE) {
        done_order[done_count] = priv->seq;
        done_count++;
    }
    dma_fence_signal(hw);
    return hw;
}

static void test_timedout_job(struct drm_sched_job *job)
{
    (void)job;
    recovery_count++;   /* callback de recovery do driver */
}

static const struct drm_sched_ops test_ops = {
    .run_job      = test_run_job,
    .timedout_job = test_timedout_job,
};

static void busy_wait_ticks(uint64_t t)
{
    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < t)
        __asm__ volatile("pause");
}

int drm_sched_test(void)
{
    int rc = -EINVAL;
    int failures = 0;

    serial_print("[DRM-SCHED] Starting scheduler tests...\n");

    /* ---- A) 1000 jobs, uma entity: completa em ordem FIFO ---- */
    done_count = 0;
    recovery_count = 0;
    make_stuck = false;
    stuck_hw_fence = NULL;

    if (drm_sched_init(&test_sched, &test_ops, "drm-sched-test", 100)) {
        sched_log("FAIL", COLOR_LIGHT_RED, "sched init failed");
        return -EINVAL;
    }
    drm_sched_entity_init(&test_ent_a, &test_sched);

    {
        static struct drm_sched_job jobs[SCHED_TEST_JOBS];
        static struct sched_test_priv privs[SCHED_TEST_JOBS];

        for (int i = 0; i < SCHED_TEST_JOBS; i++) {
            privs[i].seq = i;
            if (drm_sched_job_init(&jobs[i], &test_ent_a, &privs[i])) {
                sched_log("FAIL", COLOR_LIGHT_RED, "job_init %d falhou", i);
                goto out;
            }
            drm_sched_entity_push_job(&jobs[i], &test_ent_a);
        }

        drm_sched_flush(&test_sched);   // drena tudo (conclusão síncrona)

        if (done_count != SCHED_TEST_JOBS) {
            sched_log("FAIL", COLOR_LIGHT_RED,
                      "fila incompleta: %d/%d", done_count, SCHED_TEST_JOBS);
            goto out;
        }
        bool in_order = true;
        for (int i = 0; i < SCHED_TEST_JOBS; i++) {
            if (done_order[i] != i || jobs[i].state != DRM_SCHED_JOB_DONE ||
                !dma_fence_is_signaled(jobs[i].fence)) {
                in_order = false;
                break;
            }
        }
        if (!in_order) {
            sched_log("FAIL", COLOR_LIGHT_RED,
                      "jobs completaram fora de ordem");
            goto out;
        }
        sched_log("PASS", COLOR_LIGHT_GREEN,
                  "%d jobs completos EM ORDEM (FIFO)", SCHED_TEST_JOBS);
    }

    /* ---- B) Round-robin entre 2 entities ---- */
    drm_sched_entity_init(&test_ent_b, &test_sched);
    {
        static struct drm_sched_job b_jobs[50];
        static struct sched_test_priv b_privs[50];

        int base = done_count;
        for (int i = 0; i < 50; i++) {
            b_privs[i].seq = base + i;
            drm_sched_job_init(&b_jobs[i], &test_ent_b, &b_privs[i]);
            drm_sched_entity_push_job(&b_jobs[i], &test_ent_b);
        }
        drm_sched_flush(&test_sched);

        if (done_count != base + 50) {
            sched_log("FAIL", COLOR_LIGHT_RED, "RR: entity B não servida");
            goto out;
        }
        sched_log("PASS", COLOR_LIGHT_GREEN,
                  "round-robin entre entities OK (+50 jobs na entity B)");
    }

    /* ---- C) Timeout: job preso é abortado + recovery, pipeline segue ---- */
    drm_sched_fini(&test_sched);   // fecha instância A/B
    if (drm_sched_init(&test_sched, &test_ops, "drm-sched-timeout", 3)) {
        sched_log("FAIL", COLOR_LIGHT_RED, "sched timeout init failed");
        return -EINVAL;
    }
    drm_sched_entity_init(&test_ent_a, &test_sched);

    {
        static struct drm_sched_job stuck_job;
        static struct sched_test_priv stuck_priv;
        static struct drm_sched_job after_jobs[5];
        static struct sched_test_priv after_privs[5];

        make_stuck = true;
        done_count = 0;

        stuck_priv.seq = -1;
        drm_sched_job_init(&stuck_job, &test_ent_a, &stuck_priv);
        drm_sched_entity_push_job(&stuck_job, &test_ent_a);

        drm_sched_flush(&test_sched);   // inicia o job (fica rodando)

        if (!stuck_hw_fence || !test_sched.running) {
            sched_log("FAIL", COLOR_LIGHT_RED,
                      "job preso deveria estar RUNNING");
            goto out2;
        }

        busy_wait_ticks(5);             // excede timeout (3 ticks)

        drm_sched_flush(&test_sched);   // pump detecta o timeout

        if (recovery_count != 1 || stuck_job.state != DRM_SCHED_JOB_ABORTED) {
            sched_log("FAIL", COLOR_LIGHT_RED,
                      "timeout/recovery não disparou (rec=%d)",
                      recovery_count);
            goto out2;
        }
        if (stuck_job.fence && stuck_job.fence->error != -ETIMEDOUT) {
            sched_log("FAIL", COLOR_LIGHT_RED,
                      "fence do job abortado sem erro ETIMEDOUT");
            goto out2;
        }
        sched_log("PASS", COLOR_LIGHT_GREEN,
                  "timeout detectado → timedout_job() + abort OK");

        /* Pipeline se recupera: jobs seguintes completam */
        make_stuck = false;
        for (int i = 0; i < 5; i++) {
            after_privs[i].seq = 100 + i;
            drm_sched_job_init(&after_jobs[i], &test_ent_a, &after_privs[i]);
            drm_sched_entity_push_job(&after_jobs[i], &test_ent_a);
        }
        drm_sched_flush(&test_sched);

        bool ok2 = true;
        for (int i = 0; i < 5; i++) {
            if (after_jobs[i].state != DRM_SCHED_JOB_DONE ||
                !dma_fence_is_signaled(after_jobs[i].fence))
                ok2 = false;
        }
        if (!ok2) {
            sched_log("FAIL", COLOR_LIGHT_RED,
                      "pipeline não se recuperou após timeout");
            goto out2;
        }
        sched_log("PASS", COLOR_LIGHT_GREEN,
                  "pipeline recuperado após recovery (+5 jobs)");
        rc = 0;
        sched_log("PASS", COLOR_LIGHT_GREEN,
                  "DRM scheduler: ALL CHECKS PASSED");

out2:
        make_stuck = false;
        if (stuck_hw_fence) {
            /* job abortado já limpou a própria ref via cleanup */
            stuck_hw_fence = NULL;
        }
    }

    goto done;

out:
    failures = 1;
    goto done;

done:
    drm_sched_fini(&test_sched);
    if (rc == 0 && failures == 0)
        return 0;
    sched_log("FAIL", COLOR_LIGHT_RED, "DRM scheduler: TESTS FAILED");
    return rc ? rc : -EINVAL;
}
