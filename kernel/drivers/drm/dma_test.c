#include <drm/dma_fence.h>
#include <drm/dma_resv.h>
#include <stdlib.h>
#include <string.h>
#include <kheap.h>
#include <workqueue.h>
#include <spinlock.h>
#include <timer.h>
#include <serial.h>
#include <screen.h>
#include <stdio.h>
#include <stdarg.h>

static volatile int callback1_called = 0;
static volatile int callback2_called = 0;
static volatile int callback_order = 0;

static void test_callback1(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    (void)fence;
    (void)cb;
    callback1_called = 1;
    callback_order = 1;
    serial_print("[TEST] Callback 1 fired\n");
}

static void test_callback2(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    (void)fence;
    (void)cb;
    callback2_called = 1;
    if (callback_order == 0)
        callback_order = 2;
    serial_print("[TEST] Callback 2 fired\n");
}

static void test_callback_late(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    (void)fence;
    (void)cb;
    serial_print("[TEST] Late callback fired (fence already signaled)\n");
}

static void test_log(const char *tag, vga_color_t color, const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_print(buf);
    serial_print("\n");
    screen_log(tag, color, buf);
}

void dma_fence_test(void)
{
    serial_print("[TEST] Starting dma_fence tests...\n");
    
    spinlock_irq_t lock1 = {0}, lock2 = {0};
    spinlock_init(&lock1.lock);
    spinlock_init(&lock2.lock);
    
    struct dma_fence *fence1 = kmalloc(sizeof(struct dma_fence));
    struct dma_fence *fence2 = kmalloc(sizeof(struct dma_fence));
    
    if (!fence1 || !fence2) {
        test_log("FAIL", COLOR_LIGHT_RED, "Failed to allocate fences");
        return;
    }
    
    dma_fence_init(fence1, &dma_fence_default_ops, &lock1, 1, 100);
    dma_fence_init(fence2, &dma_fence_default_ops, &lock2, 2, 200);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Created two fences on different timelines");
    
    struct dma_fence_cb cb1 = {0}, cb2 = {0};
    
    dma_fence_add_callback(fence1, &cb1, test_callback1);
    dma_fence_add_callback(fence2, &cb2, test_callback2);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Added callbacks to both fences");
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Signaling fence1...");
    dma_fence_signal(fence1);
    
    if (callback1_called && callback_order == 1) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Callback 1 fired correctly");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Callback 1 not fired correctly");
    }
    
    callback1_called = 0;
    callback_order = 0;
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Signaling fence2...");
    dma_fence_signal(fence2);
    
    if (callback2_called && callback_order == 2) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Callback 2 fired correctly");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Callback 2 not fired correctly");
    }
    
    struct dma_fence *fence3 = kmalloc(sizeof(struct dma_fence));
    spinlock_irq_t lock3 = {0};
    spinlock_init(&lock3.lock);
    dma_fence_init(fence3, &dma_fence_default_ops, &lock3, 3, 300);
    
    struct dma_fence_cb cb3 = {0};
    dma_fence_add_callback(fence3, &cb3, test_callback_late);
    
    dma_fence_signal(fence3);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing wait on already signaled fence...");
    int ret = dma_fence_wait_timeout(fence3, false, 1000000);
    if (ret == 0) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Wait on signaled fence returned immediately");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Wait on signaled fence failed (ret=%d)", ret);
    }
    
    struct dma_fence *fence4 = kmalloc(sizeof(struct dma_fence));
    spinlock_irq_t lock4 = {0};
    spinlock_init(&lock4.lock);
    dma_fence_init(fence4, &dma_fence_default_ops, &lock4, 4, 400);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing wait timeout on unsignaled fence (0 timeout - should return immediately)...");
    ret = dma_fence_wait_timeout(fence4, false, 0);
    test_log("INFO", COLOR_LIGHT_CYAN, "Wait timeout (0) returned: %d", ret);
    if (ret == -ETIMEDOUT) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Wait timeout (0) worked correctly");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Wait timeout (0) failed (ret=%d)", ret);
    }
    
    dma_fence_put(fence1);
    dma_fence_put(fence2);
    dma_fence_put(fence3);
    dma_fence_put(fence4);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "dma_fence tests completed");
}

void dma_resv_test(void)
{
    test_log("INFO", COLOR_LIGHT_CYAN, "Starting dma_resv tests...");
    
    struct dma_resv resv;
    dma_resv_init(&resv);
    
    spinlock_irq_t lock1 = {0}, lock2 = {0}, lock3 = {0};
    spinlock_init(&lock1.lock);
    spinlock_init(&lock2.lock);
    spinlock_init(&lock3.lock);
    
    struct dma_fence *excl_fence = kmalloc(sizeof(struct dma_fence));
    struct dma_fence *shared_fence1 = kmalloc(sizeof(struct dma_fence));
    struct dma_fence *shared_fence2 = kmalloc(sizeof(struct dma_fence));
    
    dma_fence_init(excl_fence, &dma_fence_default_ops, &lock1, 10, 1000);
    dma_fence_init(shared_fence1, &dma_fence_default_ops, &lock2, 20, 2000);
    dma_fence_init(shared_fence2, &dma_fence_default_ops, &lock3, 30, 3000);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Adding exclusive fence...");
    dma_resv_add_excl_fence(&resv, excl_fence);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Adding shared fences...");
    dma_resv_add_shared_fence(&resv, shared_fence1);
    dma_resv_add_shared_fence(&resv, shared_fence2);
    
    uint32_t count = 0;
    struct dma_fence **shared = dma_resv_get_shared(&resv, &count);
    
    if (count == 2 && shared[0] == shared_fence1 && shared[1] == shared_fence2) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Shared fences retrieved correctly");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Shared fences retrieval failed");
    }
    
    for (uint32_t i = 0; i < count; i++)
        dma_fence_put(shared[i]);
    kfree(shared);
    
    struct dma_fence *excl = dma_resv_get_excl(&resv);
    if (excl == excl_fence) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Exclusive fence retrieved correctly");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Exclusive fence retrieval failed");
    }
    dma_fence_put(excl);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing unsignaled wait with 0 timeout (should return immediately)...");
    int ret = dma_resv_wait_timeout(&resv, false, 0, DMA_RESV_USAGE_WRITE);
    if (ret == -ETIMEDOUT) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Wait timeout on unsignaled exclusive fence");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Wait timeout failed (ret=%d)", ret);
    }
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Signaling exclusive fence...");
    dma_fence_signal(excl_fence);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Signaling shared fences...");
    dma_fence_signal(shared_fence1);
    dma_fence_signal(shared_fence2);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing wait on signaled exclusive fence...");
    ret = dma_resv_wait_timeout(&resv, false, 0, DMA_RESV_USAGE_WRITE);
    if (ret == 0) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Wait on signaled exclusive fence");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Wait on signaled fence failed (ret=%d)", ret);
    }
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing wait on shared fences...");
    ret = dma_resv_wait_timeout(&resv, false, 0, DMA_RESV_USAGE_READ);
    if (ret == 0) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Wait on signaled shared fences");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Wait on shared fences failed (ret=%d)", ret);
    }
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing dma_resv_test_signaled...");
    bool signaled = dma_resv_test_signaled(&resv, DMA_RESV_USAGE_WRITE | DMA_RESV_USAGE_READ);
    if (signaled) {
        test_log("PASS", COLOR_LIGHT_GREEN, "All fences signaled");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Not all fences signaled");
    }
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing dma_resv_copy_fences...");
    struct dma_resv resv2;
    dma_resv_init(&resv2);
    
    ret = dma_resv_copy_fences(&resv2, &resv);
    if (ret == 0) {
        signaled = dma_resv_test_signaled(&resv2, DMA_RESV_USAGE_WRITE | DMA_RESV_USAGE_READ);
        if (signaled) {
            test_log("PASS", COLOR_LIGHT_GREEN, "Copy fences and all signaled");
        } else {
            test_log("FAIL", COLOR_LIGHT_RED, "Copied fences not signaled");
        }
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Copy fences failed (ret=%d)", ret);
    }
    
    dma_resv_fini(&resv2);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Testing replace fences...");
    struct dma_fence *new_excl = kmalloc(sizeof(struct dma_fence));
    spinlock_irq_t lock5 = {0};
    spinlock_init(&lock5.lock);
    dma_fence_init(new_excl, &dma_fence_default_ops, &lock5, 40, 4000);
    
    dma_resv_replace_fences(&resv, new_excl);
    
    excl = dma_resv_get_excl(&resv);
    if (excl == new_excl) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Replace fences worked");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Replace fences failed");
    }
    dma_fence_put(excl);
    
    dma_fence_put(new_excl);
    dma_fence_put(excl_fence);
    dma_fence_put(shared_fence1);
    dma_fence_put(shared_fence2);
    
    dma_resv_fini(&resv);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "dma_resv tests completed");
}

void dma_fence_timeline_test(void)
{
    test_log("INFO", COLOR_LIGHT_CYAN, "Starting dma_fence timeline test...");
    
    struct dma_fence *timeline = dma_fence_alloc_timeline(42);
    if (!timeline) {
        test_log("FAIL", COLOR_LIGHT_RED, "Failed to allocate timeline");
        return;
    }
    
    if (timeline->context == 42 && timeline->seqno == 0) {
        test_log("PASS", COLOR_LIGHT_GREEN, "Timeline created with correct context");
    } else {
        test_log("FAIL", COLOR_LIGHT_RED, "Timeline context/seqno incorrect");
    }
    
    dma_fence_free_timeline(timeline);
    
    test_log("INFO", COLOR_LIGHT_CYAN, "Timeline test completed");
}

void dma_test_run_all(void)
{
    test_log("INFO", COLOR_LIGHT_CYAN, "========================================");
    test_log("INFO", COLOR_LIGHT_CYAN, "DRM DMA Fence/Reservation Object Tests");
    test_log("INFO", COLOR_LIGHT_CYAN, "========================================");
    
    dma_fence_test();
    dma_resv_test();
    dma_fence_timeline_test();
    
    test_log("INFO", COLOR_LIGHT_CYAN, "========================================");
    test_log("INFO", COLOR_LIGHT_CYAN, "All DMA tests completed");
    test_log("INFO", COLOR_LIGHT_CYAN, "========================================");
}