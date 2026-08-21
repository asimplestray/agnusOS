/*
 * thermal_monitor.c — monitor térmico da GPU (Dev 4).
 *
 * Política do GPU_PORTING_STRATEGY §4: poll de 2 s, warn em 85 °C,
 * crítico em 95 °C com PANIC. Até existir DPM real (Fase 5) o power
 * state fica forçado em LOW desde o init.
 *
 * Execução: o poll acontece no loop idle do kernel (amdgpu_idle_tick),
 * com rate-limit por ticks do PIT — a v0.1.0 não tem worker threads
 * dedicadas e o workqueue nativo é passivo (drena só em flush).
 *
 * Na emulação o SMU responde ao handshake (SMC_RESP==1) mas não
 * entrega telemetria (ARG fica 0): temp=0 significa "sem dado" e o
 * monitor opera em modo passivo — o caminho de leitura/threshold é
 * o mesmo que rodará no hardware real.
 */

#include <amdgpu.h>
#include <timer.h>
#include <string.h>
#include <serial.h>
#include <screen.h>

void amdgpu_thermal_poll_from_idle(struct amdgpu_device *adev);

int amdgpu_smu_read_temp(struct amdgpu_device *adev)
{
    /* Handshake SMC: mensagem ReadTemperature, espera RESP */
    amdgpu_wreg(adev, mmSMC_MSG_ARG, 0);
    amdgpu_wreg(adev, mmSMC_MSG, AMDGPU_SMC_MSG_ReadTemperature);

    for (int i = 0; i < 100000; i++) {
        if (amdgpu_rreg(adev, mmSMC_RESP) == AMDGPU_SMC_RESP_OK)
            break;
        __asm__ volatile("pause");
    }

    if (amdgpu_rreg(adev, mmSMC_RESP) != AMDGPU_SMC_RESP_OK)
        return -1;

    return (int)amdgpu_rreg(adev, mmSMC_MSG_ARG);   /* °C ou 0 = sem dado */
}

static void thermal_serial_dec(int v)
{
    char b[8];
    int i = 7;

    b[i] = '\0';
    do { b[--i] = '0' + v % 10; } while ((v /= 10) && i > 0);
    serial_print(&b[i]);
}

static void thermal_poll_once(struct amdgpu_device *adev)
{
    int temp = amdgpu_smu_read_temp(adev);

    if (temp > 0)
        adev->last_temp_c = temp;

    if (adev->last_temp_c <= 0)
        return;   /* modo passivo (emulação sem telemetria) */

    serial_print("[THERMAL] GPU: ");
    thermal_serial_dec(adev->last_temp_c);
    serial_print("C\n");

    if (adev->last_temp_c >= AMDGPU_THERMAL_CRIT_C) {
        adev_log("FAIL", COLOR_LIGHT_RED,
                 "GPU overtemp: %d°C — PANIC", adev->last_temp_c);
        extern void panic(const char *fmt, ...);
        panic("GPU overtemp: %dC", adev->last_temp_c);
    } else if (adev->last_temp_c >= AMDGPU_THERMAL_WARN_C) {
        adev_log("WARN", COLOR_BROWN,
                 "GPU: %d°C HOT!", adev->last_temp_c);
    }
}

/* Chamado pelo loop idle (amdgpu_idle_tick), já com rate-limit aplicado */
void amdgpu_thermal_poll_from_idle(struct amdgpu_device *adev)
{
    thermal_poll_once(adev);
}

int amdgpu_thermal_monitor_start(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev || !adev->initialized)
        return -EINVAL;
    if (adev->thermal_running)
        return 0;

    adev->thermal_running = true;
    adev->thermal_last_tick = timer_get_ticks();

    /* primeira sonda imediata (define modo passivo se temp=0) */
    int t = amdgpu_smu_read_temp(adev);
    adev_log("INFO", COLOR_LIGHT_CYAN,
             "thermal monitor ON (poll=%u ticks, warn=%u°C, crit=%u°C%s)",
             AMDGPU_THERMAL_POLL_TICKS,
             AMDGPU_THERMAL_WARN_C, AMDGPU_THERMAL_CRIT_C,
             t <= 0 ? ", SMU sem telemetria na emulacao" : "");
    return 0;
}

void amdgpu_thermal_monitor_stop(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev)
        return;
    adev->thermal_running = false;
}
