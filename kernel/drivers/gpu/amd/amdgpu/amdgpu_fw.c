/*
 * amdgpu_fw.c — Carregador de microcódigo do CP (Command Processor) para GFX8/Polaris
 *
 * Carrega e programa o firmware oficial da AMD (PFP, CE, ME) diretamente nos
 * registradores MMIO do silício Polaris (RX 470/480/570/580/590 GME).
 */

#include <amdgpu.h>
#include <firmware.h>
#include <timer.h>
#include <screen.h>
#include <serial.h>
#include <string.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

/* Header padrão AMDGPU ucode */
struct common_firmware_header {
    uint32_t size_bytes;
    uint32_t header_size_bytes;
    uint16_t header_version_major;
    uint16_t header_version_minor;
    uint16_t ip_version_major;
    uint16_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size_bytes;
    uint32_t ucode_array_offset_bytes;
    uint32_t crc32;
} __attribute__((packed));

static void fw_delay(void)
{
    for (volatile int i = 0; i < 10000; i++) {
        __asm__ volatile("pause");
    }
}

static int amdgpu_fw_upload_block(struct amdgpu_device *adev,
                                  const char *fw_name,
                                  uint32_t addr_reg,
                                  uint32_t data_reg,
                                  uint32_t *out_version)
{
    const struct firmware *fw = NULL;
    int rc = request_firmware(&fw, fw_name, adev);
    if (rc || !fw || !fw->data) {
        adev_log("FAIL", COLOR_LIGHT_RED, "Firmware nao encontrado: %s", fw_name);
        return -ENOENT;
    }

    if (fw->size < sizeof(struct common_firmware_header)) {
        adev_log("FAIL", COLOR_LIGHT_RED, "Header invalido em %s (%u bytes)", fw_name, (unsigned)fw->size);
        release_firmware(fw);
        return -EINVAL;
    }

    const struct common_firmware_header *hdr = (const struct common_firmware_header *)fw->data;
    uint32_t ucode_offset = hdr->ucode_array_offset_bytes;
    uint32_t ucode_size = hdr->ucode_size_bytes;

    if (ucode_offset + ucode_size > fw->size || (ucode_size % 4) != 0) {
        adev_log("FAIL", COLOR_LIGHT_RED, "Tamanho de ucode corrompido em %s", fw_name);
        release_firmware(fw);
        return -EINVAL;
    }

    const uint32_t *ucode_words = (const uint32_t *)(fw->data + ucode_offset);
    uint32_t num_dwords = ucode_size / 4;

    /* Configura endereco inicial como 0 */
    amdgpu_wreg(adev, addr_reg, 0);

    /* Envia todos os dwords para o registrador de dados do bloco */
    for (uint32_t i = 0; i < num_dwords; i++) {
        amdgpu_wreg(adev, data_reg, ucode_words[i]);
    }

    /* Escreve a versao no registrador de endereco para finalizar */
    amdgpu_wreg(adev, addr_reg, hdr->ucode_version);

    if (out_version)
        *out_version = hdr->ucode_version;

    adev_log("FW", COLOR_LIGHT_CYAN, "%s: v0x%x (%u dwords carregados)",
             fw_name, hdr->ucode_version, num_dwords);

    release_firmware(fw);
    return 0;
}

int amdgpu_fw_load(struct amdgpu_device *adev)
{
    if (!adev || !adev->rmmio_virt)
        return -EINVAL;

    adev_log("FW", COLOR_LIGHT_BROWN, "Carregando microcodigos do Command Processor (CP GFX8)...");

    /* 1. Coloca os motores do CP em halt antes do upload */
    amdgpu_wreg(adev, mmCP_ME_CNTL, CP_ME_CNTL_ALL_HALT);
    fw_delay();

    /* 2. Carrega o Pre-Fetch Parser (PFP) */
    int rc = amdgpu_fw_upload_block(adev, "amdgpu/polaris10_pfp.bin",
                                    mmCP_PFP_UCODE_ADDR, mmCP_PFP_UCODE_DATA,
                                    &adev->fw_pfp_ver);
    if (rc)
        goto err_unhalt;

    /* 3. Carrega o Constant Engine (CE) */
    rc = amdgpu_fw_upload_block(adev, "amdgpu/polaris10_ce.bin",
                                mmCP_CE_UCODE_ADDR, mmCP_CE_UCODE_DATA,
                                &adev->fw_ce_ver);
    if (rc)
        goto err_unhalt;

    /* 4. Carrega o Micro Engine (ME) */
    rc = amdgpu_fw_upload_block(adev, "amdgpu/polaris10_me.bin",
                                mmCP_ME_RAM_WADDR, mmCP_ME_RAM_DATA,
                                &adev->fw_me_ver);
    if (rc)
        goto err_unhalt;

    /* 5. Tira o CP de halt e inicia a execucao */
    amdgpu_wreg(adev, mmCP_ME_CNTL, 0);
    fw_delay();

    /* 6. Aguarda o CP ficar idle (pronto para comandos) */
    bool cp_idle = false;
    uint32_t grbm = 0;
    for (int i = 0; i < 100000; i++) {
        grbm = amdgpu_rreg(adev, mmGRBM_STATUS);
        if ((grbm & GRBM_STATUS_CP_BUSY) == 0) {
            cp_idle = true;
            break;
        }
        __asm__ volatile("pause");
    }

    if (!cp_idle) {
        adev_log("WARN", COLOR_LIGHT_RED, "CP timeout ao inicializar firmware (GRBM=0x%08x)", grbm);
        return -ETIMEDOUT;
    }

    adev->fw_loaded = true;
    adev_log("PASS", COLOR_LIGHT_GREEN,
             "CP ativo e pronto! PFP v0x%x | CE v0x%x | ME v0x%x (GRBM=0x%08x)",
             adev->fw_pfp_ver, adev->fw_ce_ver, adev->fw_me_ver, grbm);

    return 0;

err_unhalt:
    amdgpu_wreg(adev, mmCP_ME_CNTL, 0);
    adev_log("WARN", COLOR_LIGHT_BROWN, "CP microcode load incompleto; continuando em modo seguro.");
    return rc;
}
