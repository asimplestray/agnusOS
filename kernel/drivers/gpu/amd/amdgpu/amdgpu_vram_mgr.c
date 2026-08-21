/*
 * amdgpu_vram_mgr.c — managers de VRAM/GTT da Fase 3 (Dev 2).
 *
 * Porta o papel do amdgpu_vram_mgr/amdgpu_gtt_mgr sobre o GEM nativo
 * da Fase 1: domínio VRAM = carveout dentro do BAR1 (gerido pelo
 * drm_gem_init), domínio GTT = páginas de RAM do sistema.
 *
 * Entrega da tarefa: alocar BO em VRAM e em GTT, ler de volta padrão
 * conhecido (amdgpu_mem_selftest no boot).
 */

#include <amdgpu.h>
#include <drm/drm_gem.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

int amdgpu_vram_mgr_init(struct amdgpu_device *adev)
{
    if (!adev || !adev->vram_size)
        return -EINVAL;

    /* O gerenciador GEM passa a ser dono do carveout [vram_phys,
     * vram_phys + vram_size). BOs VRAM recebem offsets dentro dele. */
    memset(&adev->ddev, 0, sizeof(adev->ddev));
    drm_gem_init(&adev->ddev, adev->vram_phys, adev->vram_size);

    adev_log("INFO", COLOR_LIGHT_CYAN,
             "vram/gtt mgr pronto (carveout %u KB @ BAR1)",
             (unsigned)(adev->vram_size / 1024));
    return 0;
}

void amdgpu_vram_mgr_fini(struct amdgpu_device *adev)
{
    if (!adev)
        return;
    if (adev->fb_bo) {
        gem_unmap_phys(adev->fb_bo);
        gem_put(adev->fb_bo);
        adev->fb_bo = NULL;
        adev->fb_vaddr = NULL;
    }
    drm_gem_fini(&adev->ddev);
}

int amdgpu_bo_create(struct amdgpu_device *adev, size_t size, bool in_vram,
                     struct drm_gem_object **out)
{
    uint32_t domain = in_vram ? DRM_GEM_DOMAIN_VRAM : DRM_GEM_DOMAIN_GTT;

    if (!adev || !out)
        return -EINVAL;
    return gem_create(&adev->ddev, size, domain, out);
}

/* ------------------------------------------------------------------ */
/* Selftest Dev 2: BO em VRAM + BO em GTT com read-back                */
/* ------------------------------------------------------------------ */

#define MGR_TEST_BO_SIZE (8 * 4096)

int amdgpu_mem_selftest(void)
{
    struct amdgpu_device *adev = amdgpu_adev;
    struct drm_gem_object *bo_vram = NULL, *bo_gtt = NULL;
    void *v1, *v2;
    int rc = -EINVAL;
    bool ok;

    if (!adev || !adev->initialized)
        return -EINVAL;

    serial_print("[amdgpu] mem selftest: VRAM + GTT BOs\n");

    /* O FB do modeset pode já estar no carveout — compara por delta */
    size_t base_used = drm_gem_vram_used(&adev->ddev);

    rc = amdgpu_bo_create(adev, MGR_TEST_BO_SIZE, true, &bo_vram);
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "BO VRAM falhou (%d)", rc);
        goto out;
    }
    rc = amdgpu_bo_create(adev, MGR_TEST_BO_SIZE, false, &bo_gtt);
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "BO GTT falhou (%d)", rc);
        goto out;
    }

    v1 = gem_mmap_phys(bo_vram);
    v2 = gem_mmap_phys(bo_gtt);
    if (!v1 || !v2) {
        adev_log("FAIL", COLOR_LIGHT_RED, "mmap dos BOs falhou");
        rc = -EINVAL;
        goto out;
    }

    /* VRAM: padrão A */
    for (uint32_t i = 0; i < MGR_TEST_BO_SIZE / 4; i++)
        ((uint32_t *)v1)[i] = 0xA1000000u + i;
    ok = true;
    for (uint32_t i = 0; i < MGR_TEST_BO_SIZE / 4; i++)
        if (((uint32_t *)v1)[i] != 0xA1000000u + i) { ok = false; break; }

    /* GTT: padrão B */
    for (uint32_t i = 0; i < MGR_TEST_BO_SIZE / 4; i++)
        ((uint32_t *)v2)[i] = 0xB2000000u + i;
    for (uint32_t i = 0; i < MGR_TEST_BO_SIZE / 4; i++)
        if (((uint32_t *)v2)[i] != 0xB2000000u + i) { ok = false; break; }

    /* Contabilidade do carveout: BO VRAM somou ao uso pré-existente */
    size_t used = drm_gem_vram_used(&adev->ddev);
    ok = ok && (used >= base_used + MGR_TEST_BO_SIZE);

    gem_unmap_phys(bo_gtt);
    gem_unmap_phys(bo_vram);

    if (!ok) {
        adev_log("FAIL", COLOR_LIGHT_RED,
                 "read-back/contabilidade divergente");
        rc = -EINVAL;
        goto out;
    }

    gem_put(bo_gtt); bo_gtt = NULL;
    gem_put(bo_vram); bo_vram = NULL;

    if (drm_gem_vram_used(&adev->ddev) != base_used) {
        adev_log("FAIL", COLOR_LIGHT_RED, "carveout não voltou ao baseline");
        rc = -EINVAL;
        goto out;
    }

    adev_log("PASS", COLOR_LIGHT_GREEN,
             "mem selftest OK (VRAM+GTT read-back, carveout limpo)");
    rc = 0;

out:
    if (bo_gtt) { gem_unmap_phys(bo_gtt); gem_put(bo_gtt); }
    if (bo_vram) { gem_unmap_phys(bo_vram); gem_put(bo_vram); }
    return rc;
}
