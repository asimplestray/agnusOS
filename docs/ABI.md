# AgnusOS uAPI 1.0

> A fonte canônica da ABI pública é `kernel/include/uapi/`. Números existentes nunca podem ser reutilizados ou deslocados. Estruturas públicas só podem ser estendidas por campos versionados e reservados, ou substituídas por uma nova operação.

## Versionamento

| Componente | Header | Versão |
|---|---|---|
| DRM core | `kernel/include/uapi/drm.h` | `1.0` |
| AgnusOS traps | `kernel/include/uapi/aos.h` | `1.0` |

`DRM_UAPI_VERSION_MAJOR` e `AOS_UAPI_VERSION_MAJOR` — bump major quebra ABI, minor adiciona compat.

## Mapa

```
kernel/include/uapi/
  drm.h          — DRM_IOCTL_BASE 'd', ioctls esparsos 0x00,0x02,0x40-0x43,0xA0-0xA3 (VERSION/GET_MAGIC/GEM/PRIME/KMS atomic) + GEM_DOMAIN_* + drm_version
  aos.h          — mapa canônico de 66 traps 0-65, flags, Assign e MsgPort

kernel/include/
  drm/           — internal (não uAPI): drm_device.h, drm_gem.h (não citar contagem de linhas aqui — muda sempre)
  compat/        — thin wrappers linux_*.h (não exposto)
```

## Regras

1. **Mapa único:** `kernel/include/uapi/aos.h` é a única definição numérica das traps. O header interno apenas o inclui.
2. **Extensão por append:** as traps 0 a 60 permanecem nos números publicados. Memory pools ocupam 61 a 65.
3. **Layouts protegidos:** `aos_msg` possui payload de 128 bytes e tamanho total de 140 bytes, verificado por `_Static_assert`.
4. **Flags estáveis:** os valores de `AOS_O_*` existem somente no header público e são consumidos pelo kernel sem uma segunda definição conflitante.
5. **Ioctls estáveis:** DRM usa codificação `_IOC` compatível com Linux. O dispatcher extrai o número do comando antes de consultar suas tabelas.
6. **Compatibilidade de fonte:** aliases `SYS_*` preservam a compilação de aplicações antigas. Eles não constituem um segundo mapa em runtime.

## Uso

```c
#include <uapi/drm.h>
#include <uapi/aos.h>

int fd = aos_open("Work:docs/readme", AOS_O_RDONLY, frame); // Assign expande Work: -> /fat32
int port = aos_create_port("MyPort", frame);
struct aos_msg m = {.code=1, .size=4}; memcpy(m.payload, "hi", 4);
aos_put_msg(port, &m, frame);

struct drm_gem_create gc = {.size=4096, .domain=GEM_DOMAIN_VRAM};
ioctl(fd, DRM_IOCTL_GEM_CREATE, &gc); // handle em gc.handle
```

> Assinatura real: `aos_open(name, mode, frame)` — 3 args (`kernel/include/syscall.h`, `kernel/syscall.c`). `AOS_SetProcGroup/GetProcGroup/SetConProc/GetConProc` (28-31) publicados mas sem dispatch (retornam `NOT_FOUND`).

## Futuro porte de drivers Linux

- Drivers importados devem consumir contratos públicos ou a camada de compatibilidade, sem depender acidentalmente de detalhes internos do AgnusOS.
- `compat/linux_*.h` em `kernel/include/compat/` é uma camada privada do kernel e não faz parte da uAPI.
- Compatibilidade com libdrm ou drivers específicos só deve ser anunciada depois de testes userspace automatizados passarem.

## Política de release

- Uma tag `uapi-X.Y` só pode ser criada após build limpo e testes ABI userspace.
- Alterações incompatíveis exigem aumento de `MAJOR` e um período explícito de migração.
- Novas operações compatíveis incrementam `MINOR` e usam números ainda não publicados.
- Cada release deve arquivar os headers públicos e os resultados dos testes de layout e ioctl.

## Verificação automatizada

Execute `make abi-check` antes de qualquer release. O teste userspace
`tests/uapi_abi.c` compila os headers públicos ativos juntos e trava um
subset do contrato: traps-chave (`Exit/DoIO/FindTask/Assign/ReplyMsg/Select/
RecvFrom/CreatePool/PoolAvail/NR`), 3 aliases `SYS_*`, flags `AOS_O_*`,
números de ioctls DRM e 2 layouts DRM. Não cobre as 66 traps — cobertura total
ainda pendente (ver `docs/PRODUCTION_READINESS.md`).

Esse teste protege o contrato já publicado, mas não substitui testes funcionais
abrindo `/dev/dri/cardN` e exercitando GEM, PRIME e KMS.
