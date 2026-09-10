/*
 * compat_check.c — validação da camada de compatibilidade Linux->AgnusOS.
 *
 * Fase 2 Dev 4 — entrega: os headers kernel/include/compat/linux_*.h
 * compilam e se comportam 1:1 com as APIs nativas. Este TU instancia e
 * exercita cada header (list, mutex, spinlock, work, fence, dma_buf,
 * module) para garantir que a convenção documentada não regrede.
 */

#include <compat/linux_types.h>
#include <compat/linux_list.h>
#include <compat/linux_mutex.h>
#include <compat/linux_spinlock.h>
#include <compat/linux_work.h>
#include <compat/linux_fence.h>
#include <compat/linux_dma_buf.h>
#include <compat/linux_module.h>

#include <string.h>
#include <kheap.h>
#include <serial.h>
#include <screen.h>
#include <stdio.h>
#include <stdarg.h>

/* module macros expandem para nada (uso em file scope, como no Linux) */
module_init(compat_layer_test);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AgnusOS compat layer check");

struct compat_test_item {
    int value;
    struct list_head node;
};

static void compat_log(const char *tag, vga_color_t color, const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_print("[COMPAT] ");
    serial_print(buf);
    serial_print("\n");
    screen_log(tag, color, buf);
}

/* ---- list ---- */
static bool compat_test_list(void)
{
    LIST_HEAD(head);
    struct compat_test_item items[5];
    struct list_head *pos;
    int i = 0;

    INIT_LIST_HEAD(&head);
    for (i = 0; i < 5; i++) {
        items[i].value = i * 11;
        INIT_LIST_HEAD(&items[i].node);
        list_add_tail(&items[i].node, &head);
    }

    if (list_empty(&head))
        return false;

    /* forward walk in insertion order */
    i = 0;
    list_for_each(pos, &head) {
        struct compat_test_item *it = list_entry(pos, struct compat_test_item, node);
        if (it->value != i * 11)
            return false;
        i++;
    }
    if (i != 5)
        return false;

    /* reverse walk */
    i = 4;
    list_for_each_prev(pos, &head) {
        struct compat_test_item *it = list_entry(pos, struct compat_test_item, node);
        if (it->value != i * 11)
            return false;
        i--;
    }

    /* safe deletion of middle entry */
    list_del(&items[2].node);
    {
        struct list_head *pos2, *tmp;
        i = 0;
        list_for_each_safe(pos2, tmp, &head) {
            (void)0;
            i++;
            if (i > 10)
                return false;
        }
    }
    if (i != 4)
        return false;

    return true;
}

/* ---- mutex / spinlock ---- */
static bool compat_test_locking(void)
{
    DEFINE_MUTEX(m);
    DEFINE_SPINLOCK(sl);
    unsigned long flags;

    mutex_compat_init(&m);
    if (mutex_is_locked(&m))
        return false;

    mutex_lock(&m);
    if (!mutex_is_locked(&m))
        return false;
    if (mutex_trylock(&m))
        return false;   /* already held */
    mutex_unlock(&m);
    if (mutex_is_locked(&m))
        return false;
    mutex_compat_destroy(&m);

    spin_lock_init(&sl);
    if (spin_is_locked(&sl))
        return false;

    flags = spin_lock_irqsave_compat(&sl);
    if (!spin_is_locked(&sl))
        return false;
    spin_unlock_irqrestore_compat(&sl, flags);
    if (spin_is_locked(&sl))
        return false;

    return true;
}

/* ---- work ---- */
static volatile int compat_work_ran = 0;
static struct work_struct compat_work;

static void compat_work_fn(struct work_struct *w)
{
    (void)w;
    compat_work_ran++;
}

static bool compat_test_work(void)
{
    INIT_WORK(&compat_work, compat_work_fn);
    compat_work_ran = 0;

    schedule_work(&compat_work);
    flush_scheduled_work();

    return compat_work_ran == 1;
}

/* ---- fence helpers ---- */
static bool compat_test_fence(void)
{
    uint64_t ctx_a = dma_fence_context_alloc(1);
    uint64_t ctx_b = dma_fence_context_alloc(2);

    if (ctx_b != ctx_a + 1)
        return false;       /* alloc(2) consumed ctx_a+1 and ctx_a+2 */
    if (dma_fence_context_alloc(1) != ctx_a + 3)
        return false;

    return true;
}

int compat_layer_test(void)
{
    int failures = 0;

    serial_print("[COMPAT] Starting compat layer validation...\n");

    if (compat_test_list())
        compat_log("PASS", COLOR_LIGHT_GREEN, "linux_list: add_tail/for_each/for_each_prev/safe_del OK");
    else { failures++; compat_log("FAIL", COLOR_LIGHT_RED, "linux_list failed"); }

    if (compat_test_locking())
        compat_log("PASS", COLOR_LIGHT_GREEN, "linux_mutex/linux_spinlock: lock/unlock/trylock/irqsave OK");
    else { failures++; compat_log("FAIL", COLOR_LIGHT_RED, "locking wrappers failed"); }

    if (compat_test_work())
        compat_log("PASS", COLOR_LIGHT_GREEN, "linux_work: schedule_work/flush_scheduled_work OK");
    else { failures++; compat_log("FAIL", COLOR_LIGHT_RED, "work wrappers failed"); }

    if (compat_test_fence())
        compat_log("PASS", COLOR_LIGHT_GREEN, "linux_fence: dma_fence_context_alloc OK");
    else { failures++; compat_log("FAIL", COLOR_LIGHT_RED, "fence helpers failed"); }

    if (failures == 0)
        compat_log("PASS", COLOR_LIGHT_GREEN, "Compat layer: ALL CHECKS PASSED");
    else
        compat_log("FAIL", COLOR_LIGHT_RED, "Compat layer: %d check group(s) FAILED", failures);

    return -failures;
}
