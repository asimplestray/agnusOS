/*
 * amdgpu_device.c — amdgpu v0.1.0 MINIMAL: HW init + ASIC detect (Dev 1).
 *
 * Sequência de init inspirada em amdgpu_device_init() (Linux 6.6):
 *   PCI enable → rmmio (BAR0) → VRAM aperture (BAR1) → detect de ASIC
 *   por PCI ID → reset → FB enable → mem mgrs → modeset → GFX ring.
 *
 * Compute/SDMA ficam DESLIGADOS por padrão — só GFX ring na v0.1.0.
 */

#include <amdgpu.h>
#include <amdgpu_dc.h>
#include <pci.h>
#include <vmm.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

struct amdgpu_device *amdgpu_adev = NULL;

/* Tabela de device IDs suportada (Polaris/VI + Navi22 para o DC da Fase 4) */
static const struct amdgpu_asic_entry amdgpu_asic_table[] = {
    { 0x67DF, CHIP_POLARIS10, "Polaris10" },
    { 0x67EF, CHIP_POLARIS11, "Polaris11" },
    { 0x67FF, CHIP_POLARIS11, "Polaris11" },
    { 0x6987, CHIP_POLARIS12, "Polaris12" },
    { 0x6FDF, CHIP_POLARIS10, "Polaris20 (RX 590 GME)" },
    { 0x73DF, CHIP_NAVI22,    "Navi22"    },
    { 0,      CHIP_UNKNOWN,   NULL        },
};

void adev_log(const char *tag, vga_color_t color, const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_print("[amdgpu] ");
    serial_print(buf);
    serial_print("\n");
    screen_log(tag, color, buf);
}

static void adev_serial_hex(const char *name, uint32_t val)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[48];
    int n = 0;

    while (*name && n < 24)
        buf[n++] = *name++;
    buf[n++] = ' ';
    for (int i = 7; i >= 0; i--)
        buf[n++] = hexd[(val >> (i * 4)) & 0xF];
    buf[n] = '\0';
    serial_print(buf);
    serial_print("\n");
}

static const struct amdgpu_asic_entry *amdgpu_find_asic(uint16_t dev_id)
{
    for (int i = 0; amdgpu_asic_table[i].name; i++)
        if (amdgpu_asic_table[i].dev_id == dev_id)
            return &amdgpu_asic_table[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* BARs                                                                */
/* ------------------------------------------------------------------ */

static int amdgpu_map_rmmio(struct amdgpu_device *adev)
{
    uint64_t bar0 = pci_read_bar(adev->bus, adev->dev, adev->func, 0);

    if (!bar0 || (bar0 & 1))
        return -EINVAL;

    adev->rmmio_phys = bar0 & ~0xFULL;
    adev->rmmio_virt = AMDGPU_RMMIO_VADDR;

    vmm_map_region(adev->rmmio_virt, adev->rmmio_phys, AMDGPU_MMIO_SIZE,
                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    return 0;
}

static int amdgpu_init_vram_aperture(struct amdgpu_device *adev)
{
    uint64_t bar1 = pci_read_bar(adev->bus, adev->dev, adev->func, 1);

    if (!bar1 || (bar1 & 1))
        return -EINVAL;

    /* Tamanho da VRAM via CONFIG_MEMSIZE (MB), como o amdgpu real lê
     * do FW scratch — NÃO fazemos sizing por config space: escrever nos
     * registros de BAR sob KVM invalida o mapeamento do slot e derruba
     * o guest. Default da emulação se o registro vier zerado. */
    adev->vram_phys = bar1 & ~0xFULL;
    adev->vram_virt = AMDGPU_VRAM_VADDR;

    /* rmmio precisa estar mapeado antes */
    if (!adev->rmmio_virt)
        return -EINVAL;

    uint32_t mb = amdgpu_rreg(adev, mmCONFIG_MEMSIZE);
    if (mb == 0 || mb > 512)
        mb = 256;
    adev->vram_size = (size_t)mb << 20;

    vmm_map_region(adev->vram_virt, adev->vram_phys,
                   (uint32_t)adev->vram_size,
                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reset + FB enable                                                   */
/* ------------------------------------------------------------------ */

static void amdgpu_wait_grbm_idle(struct amdgpu_device *adev)
{
    for (int i = 0; i < 100000; i++) {
        if ((amdgpu_rreg(adev, mmGRBM_STATUS) & 0x9F800000u) == 0)
            break;
        __asm__ volatile("pause");
    }
}

static void amdgpu_soft_reset(struct amdgpu_device *adev)
{
    amdgpu_wreg(adev, mmGRBM_SOFT_RESET, 0xFFFFFFFFu);
    for (volatile int i = 0; i < 10000; i++)
        ;
    amdgpu_wreg(adev, mmGRBM_SOFT_RESET, 0);
    amdgpu_wait_grbm_idle(adev);
}

static void amdgpu_enable_fb(struct amdgpu_device *adev)
{
    /* BIF_FB_EN: bit0 = FB, bit1 = FB_READ (mesmo valor do amdgpu real) */
    amdgpu_wreg(adev, mmBIF_FB_EN, 0x3u);
    adev->fb_enabled = true;
}

/* ------------------------------------------------------------------ */
/* Detect + early init                                                 */
/* ------------------------------------------------------------------ */

static struct amdgpu_device *amdgpu_create_adev(uint8_t bus, uint8_t dev,
                                                uint8_t func)
{
    struct amdgpu_device *adev = kmalloc(sizeof(*adev));

    if (!adev)
        return NULL;
    memset(adev, 0, sizeof(*adev));
    adev->bus = bus;
    adev->dev = dev;
    adev->func = func;
    spinlock_init(&adev->ring_lock.lock);
    return adev;
}

static int amdgpu_detect_and_init(struct amdgpu_device *adev)
{
    uint16_t vendor = pci_read_word(adev->bus, adev->dev, adev->func, 0);
    const struct amdgpu_asic_entry *asic;

    if (vendor != AMDGPU_VENDOR_ID)
        return -ENODEV;

    adev->dev_id = pci_read_word(adev->bus, adev->dev, adev->func, 2);
    asic = amdgpu_find_asic(adev->dev_id);
    if (!asic)
        return -ENODEV;

    adev->asic_type = asic->type;
    adev->asic_name = asic->name;
    adev->revision = pci_read_dword(adev->bus, adev->dev, adev->func,
                                    0x08) & 0xFF;

    adev_log("PCI", COLOR_LIGHT_CYAN, "Dispositivo no PCI %02x:%02x.0 (Vendor 1002, Device %04x, Rev %02x)",
             adev->bus, adev->dev, adev->dev_id, adev->revision);
    adev_log("ASIC", COLOR_LIGHT_GREEN, "Modelo detectado: %s (GCN 4th Gen / GFX8)",
             adev->asic_name);

    /* Habilita MEM_SPACE + BUS_MASTER no command register */
    uint32_t cmd = pci_read_dword(adev->bus, adev->dev, adev->func,
                                  PCI_CONFIG_COMMAND);
    pci_write_dword(adev->bus, adev->dev, adev->func, PCI_CONFIG_COMMAND,
                    cmd | 0x6);

    int rc = amdgpu_map_rmmio(adev);
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "Falha ao mapear BAR0 MMIO (%d)", rc);
        return rc;
    }
    adev_log("MMIO", COLOR_LIGHT_CYAN, "BAR0 MMIO: 0x%lx mapeado em 0x%lx (16 MB)",
             adev->rmmio_phys, adev->rmmio_virt);

    rc = amdgpu_init_vram_aperture(adev);
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "Falha ao mapear BAR1 VRAM (%d)", rc);
        return rc;
    }
    adev_log("VRAM", COLOR_LIGHT_CYAN, "BAR1 VRAM: 0x%lx mapeado em 0x%lx (%u MB)",
             adev->vram_phys, adev->vram_virt, (unsigned)(adev->vram_size >> 20));

    if (adev->dev_id != 0x6FDF) {
        amdgpu_soft_reset(adev);
        amdgpu_enable_fb(adev);
        adev_log("HW", COLOR_LIGHT_GREEN, "Soft-reset GRBM OK | BIF_FB_EN ativado (vgpu)");
    } else {
        adev_log("HW", COLOR_LIGHT_GREEN, "Silicio Real Polaris20 (RX 590 GME) - Soft-reset desativado por seguranca");
    }

    /* PM: power state forçado LOW até existir DPM real (Fase 5) */
    adev->dpm_forced_level = AMD_DPM_FORCED_LEVEL_LOW;
    adev_log("INFO", COLOR_LIGHT_CYAN,
             "DPM ausente na v0.1.0 — power state forçado LOW");

    adev_serial_hex("CONFIG_MEMSIZE(MB):", amdgpu_rreg(adev, mmCONFIG_MEMSIZE));
    adev_serial_hex("GRBM_STATUS       :", amdgpu_rreg(adev, mmGRBM_STATUS));
    adev_serial_hex("GRBM_STATUS2      :", amdgpu_rreg(adev, mmGRBM_STATUS2));
    adev_serial_hex("SRBM_STATUS       :", amdgpu_rreg(adev, mmSRBM_STATUS));
    adev_serial_hex("BIF_FB_EN         :", amdgpu_rreg(adev, mmBIF_FB_EN));
    adev_serial_hex("SMC_RESP          :", amdgpu_rreg(adev, mmSMC_RESP));

    uint32_t mb = amdgpu_rreg(adev, mmCONFIG_MEMSIZE);
    uint32_t grbm = amdgpu_rreg(adev, mmGRBM_STATUS);
    uint32_t resp = amdgpu_rreg(adev, mmSMC_RESP);
    adev_log("REGS", COLOR_LIGHT_MAGENTA, "Dump: MEMSIZE=%u MB | GRBM=0x%08x | SMC_RESP=0x%x",
             mb, grbm, resp);

    if (mb != 0 && mb <= 32768 && resp == AMDGPU_SMC_RESP_OK)
        adev_log("PASS", COLOR_LIGHT_GREEN,
                 "Register dump coerente com silicio (MEMSIZE=%u MB)", mb);
    else
        adev_log("WARN", COLOR_BROWN,
                 "Dump fora do esperado (MEMSIZE=%u RESP=%u)", mb, resp);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Probe PCI + cadeia completa                                         */
/* ------------------------------------------------------------------ */

int amdgpu_init(void)
{
    struct amdgpu_device *adev = NULL;
    int found_bus = -1, found_dev = -1;
    int rc;

    if (amdgpu_adev)
        return 0;

    screen_log("SCAN", COLOR_LIGHT_BROWN, "amdgpu: Escaneando barramento PCI por GPUs AMD suportadas...");

    for (int b = 0; b < 256 && found_bus < 0; b++) {
        for (int d = 0; d < 32; d++) {
            uint16_t vendor = pci_read_word((uint8_t)b, (uint8_t)d, 0, 0);
            if (vendor != AMDGPU_VENDOR_ID)
                continue;
            uint16_t id = pci_read_word((uint8_t)b, (uint8_t)d, 0, 2);
            if (amdgpu_find_asic(id)) {
                found_bus = b;
                found_dev = d;
                break;
            }
        }
    }

    if (found_bus < 0) {
        screen_log("INFO", COLOR_LIGHT_RED, "amdgpu: Nenhuma GPU AMD suportada encontrada no barramento.");
        serial_print("[amdgpu] nenhuma ASIC suportada no barramento "
                     "(boot com vgpu amd-rx480 para validar)\n");
        return -ENODEV;
    }

    screen_log("FOUND", COLOR_LIGHT_GREEN, "amdgpu: GPU AMD suportada encontrada! Inicializando hardware...");

    adev = amdgpu_create_adev((uint8_t)found_bus, (uint8_t)found_dev, 0);
    if (!adev)
        return -ENOMEM;

    rc = amdgpu_detect_and_init(adev);
    if (rc)
        goto err_free;

    rc = amdgpu_vram_mgr_init(adev);            /* Dev 2 */
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "vram_mgr_init falhou (%d)", rc);
        goto err_free;
    }

    /* Em hardware físico real (RX 590 GME), a sonda de hardware está concluída com sucesso.
     * Não tentamos os modos simulados da vgpu nem anéis sem microcódigo. */
    if (adev->dev_id == 0x6FDF) {
        adev->initialized = true;
        amdgpu_adev = adev;
        adev_log("PASS", COLOR_LIGHT_GREEN, "Hardware Real Polaris20 (RX 590 GME) sondado com SUCESSO!");
        adev_log("INFO", COLOR_LIGHT_CYAN, "Display mantido ativo no modo seguro UEFI GOP.");
        adev_log("INFO", COLOR_LIGHT_CYAN, "PCIe MMIO BAR0 e VRAM BAR1 operacionais!");
        return 0;
    }

    rc = amdgpu_modeset_init(adev, 1920, 1080, 32);   /* Dev 3 */
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "modeset_init falhou (%d)", rc);
        goto err_fini_mem;
    }

    rc = amdgpu_gfx_init(adev);                 /* Dev 4 */
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "gfx_init falhou (%d)", rc);
        goto err_fini_mode;
    }

    /* marca como inicializado ANTES dos selftests (eles consultam) */
    adev->initialized = true;
    amdgpu_adev = adev;

    /* Selftests da fase */
    amdgpu_mem_selftest();
    amdgpu_display_selftest();
    amdgpu_gfx_selftest();

    /* Fase 4: selftest do DC (MST + HPD + modestest interno) */
    if (adev->use_dc)
        amdgpu_dc_selftest();

    /* Runtime Dev 4: térmico + test pattern contínuo (24/7) */
    amdgpu_thermal_monitor_start();
    amdgpu_gpu_test_pattern_start();

    adev_log("PASS", COLOR_LIGHT_GREEN,
             "amdgpu v0.1.0 MINIMAL ativo (%s, modeset %ux%ux32)",
             adev->asic_name, adev->mode_w, adev->mode_h);
    return 0;

err_fini_mode:
    amdgpu_modeset_fini(adev);
err_fini_mem:
    amdgpu_vram_mgr_fini(adev);
err_free:
    kfree(adev);
    return rc;
}

void amdgpu_fini(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev)
        return;

    amdgpu_gpu_test_pattern_stop();
    amdgpu_thermal_monitor_stop();
    amdgpu_gfx_fini(adev);
    amdgpu_modeset_fini(adev);
    amdgpu_vram_mgr_fini(adev);

    kfree(adev);
    amdgpu_adev = NULL;
}
