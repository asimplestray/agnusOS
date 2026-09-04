# ApolloOS uAPI 1.0 — ABI estável antes do porte amdgpu 6.6

> **Congelado:** `kernel/include/uapi/` `uapi-1.0` `2026-09-04` `PRE_AMDGPU_TASKS.md:P3` **antes** de copiar `drivers/gpu/drm/amd` `6.6`. Regra: nunca quebrar número/struct; extensão via `version`/`pad` ou novo `ioctl`.

## Versionamento

| Componente | Header | Versão | Tag |
|---|---|---|---|
| DRM core | `kernel/include/uapi/drm.h` | `1.0` | `uapi-1.0` |
| amdgpu | `kernel/include/uapi/amdgpu_drm.h` | `1.0` | `uapi-1.0` |
| AOS traps | `kernel/include/uapi/aos.h` | `1.0` | `uapi-1.0` |

`DRM_UAPI_VERSION_MAJOR` `AMDGPU_UAPI_VERSION_MAJOR` `AOS_UAPI_VERSION_MAJOR` — bump major quebra ABI, minor adiciona compat.

## Mapa

```
kernel/include/uapi/
  drm.h          — DRM_IOCTL_BASE 'd' 0x00-0xA3 (VERSION/GEM/PRIME/KMS atomic) + GEM_DOMAIN_* + drm_version
  amdgpu_drm.h   — CHIP 0x67DF/0x6FDF/0x73DF + GEM_CREATE + GFX PACKET3 + FW blobs + DC modes + AMDGPU_INFO addrs
  aos.h          — AOS_0..52 traps + O_RDONLY/CREAT + Assign/MsgPort structs

kernel/include/
  drm/           — internal (não uAPI): drm_device.h:69, drm_gem.h:22
  compat/        — thin wrappers linux_*.h (não exposto)
```

## Regras

1. **Numeric ABI idêntico** `c32c427`: `AOS_Exit 0` == antigo `SYS_EXIT 0` (`kernel/include/syscall.h:109` compat `SYS_*` alias), `46-52` `Assign/MsgPort` novos em `c32c427` (`f9b3068`).
2. **Structs versionadas:** `drm_version` tem `name_len/date_len`, `drm_amdgpu_gem_create_in` tem `alignment/flags/pad`, `aos_msg` `128B` fixo.
3. **Ioctl nunca muda número:** `DRM_IOCTL_GEM_CREATE 0x40` + `DRM_AMDGPU_GEM_CREATE 0x40` etc. Novos em `0x41+`.
4. **Compat:** `kernel/include/uapi/aos.h` define `SYS_*` aliases para código antigo compilar; `kernel/syscall.c:34` dispacha `AOS_*` e `SYS_*` mesmo handler (`aos_*`).
5. **Firmware:** `AMDGPU_FW_*` `amdgpu/polaris10_* 17044` `scripts/make_fw_initrd.sh:8` versão `v0xeb` documentada, não ABI mas `uapi` lista nomes estáveis.

## Uso

```c
#include <uapi/drm.h>
#include <uapi/amdgpu_drm.h>
#include <uapi/aos.h>

int fd = aos_open("Work:docs/readme", AOS_O_RDONLY, 0, frame); // Assign expande Work: -> /fat32
int port = aos_create_port("MyPort", frame);
struct aos_msg m = {.code=1, .size=4}; memcpy(m.payload, "hi", 4);
aos_put_msg(port, &m, frame);

struct drm_gem_create gc = {.size=4096, .domain=GEM_DOMAIN_VRAM};
ioctl(fd, DRM_IOCTL_GEM_CREATE, &gc); // handle em gc.handle
```

## Compat com porte amdgpu 6.6

- `drivers/gpu/drm/amd` `6.6` deve incluir apenas `uapi/*.h`, nunca `kernel/include/drm/*` internal.
- `compat/linux_*.h` `kernel/include/compat/` mapeia `dma_fence` `workqueue` para `drm_sched` sem vazar `kmalloc` — `GPU_PORTING_STRATEGY.md:82`.
- Teste `libdrm` `amdgpu` `radv` contra `uapi-1.0` headers antes do `port`.

## Histórico

- `f18c8f4` Unix `46` `SYS_*`
- `c32c427` Amiga `53` `AOS_*` + `Assign/MsgPort` `f9b3068` `NR 53`
- `uapi-1.0` congela `53` + `GEM/PRIME/KMS` + `amdgpu` `1.0`
