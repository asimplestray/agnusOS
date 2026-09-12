# AMDGPU stub descontinuado

Este diretório preserva o protótipo AMDGPU desenvolvido para validar a
infraestrutura gráfica e os dispositivos AMD do ambiente vGPU do AgnusOS.
Ele é mantido apenas como referência histórica e não participa do build nem
da inicialização normal do kernel.

## Estado arquivado

O protótipo contém:

- detecção PCI para variantes Polaris (`0x67DF/0x67EF/0x67FF`) e Navi22 (`0x73DF`) usadas nos testes;
- gerenciadores mínimos de VRAM e GTT sobre GEM;
- ring GFX emulado com fences e scheduler DRM;
- carregador de firmware Polaris PFP, CE e ME (só esses 3 — `make_fw_initrd.sh` lista 8 blobs, `mec/rlc/sdma/sdma1/mc` nunca carregados);
- modeset, Display Core estrutural, DCE/DCN, HPD e MST simplificado;
- framebuffer duplo, page flip e padrão de teste animado;
- monitor térmico e selftests de memória, display, GFX e DC.

## Limitações

Este código não é um porte do driver AMDGPU do Linux e não deve ser usado
como driver para hardware real. Diversos comportamentos dependem do modelo
vGPU, incluindo a drenagem do ring em CPU, os registradores simplificados de
scanout, os timings fixos e a ausência de inicialização completa dos blocos
de IP da GPU.

## Organização

- `amdgpu/`: implementação arquivada do stub;
- `include/`: headers internos usados pelo stub;
- `include/uapi/amdgpu_drm.h`: antiga uAPI experimental do protótipo.
- `make_fw_initrd.sh`: antigo empacotador de firmware Polaris usado pelo stub.

Os headers foram movidos junto com a implementação para impedir que código
ativo do kernel dependa acidentalmente dessa interface descontinuada.

## Como consultar ou recompilar

O código pode ser estudado e reaproveitado, mas uma recompilação isolada
exige adicionar temporariamente este diretório e `include/` ao caminho de
includes e restaurar os objetos correspondentes no Makefile. Essa operação
não faz parte da configuração suportada do AgnusOS.

## Direção futura

A infraestrutura genérica de GPU permanece no kernel: DRM core, GEM,
dma-buf, dma-fence, dma-resv, scheduler DRM, atomic KMS, PCI, DMA, IOMMU.
O driver `gpu/agnus/` (`agnus_drv.c` + `polaris.c`) compila/linkado mas sem
probe/init — efetivamente inativo, usado como bring-up local Polaris.
Um futuro driver AMD será novo sobre esses contratos (não restauração deste
stub) e sobre uma camada de compatibilidade explicitamente definida.

## Estratégia de reuso (anotação)

- `kernel/drivers/gpu/descontinued/amdgpu_stub/`: protótipo arquivado. Futuro
  `amdgpu` será implementação nova sobre DRM/GEM, não restauração. Não evoluir aqui.
- `kernel/drivers/gpu/agnus/` (compila, sem probe — inativo hoje): NÃO vai para
  `descontinued`. Jogar fora seria perder trabalho. Será modificado e reaproveitado.
- Polaris primeiro porque é o hardware real do desenvolvedor (mais fácil de
  testar MMIO/BAR/modeset). `vgpu/scripts/run-test.sh` serve para CI (atenção:
  `QEMU_BIN` absoluto no script, quebra fora da máquina).
- Regra de reuso: manter a casca (`polaris_init/set_mode/fill_rect` em
  `polaris.h`) e trocar o miolo quando chegar o driver real. Hoje
  `polaris_set_mode()` ignora `w/h` (timings hard `2200/1125`) e o modo é fixo
  `1920x1080x32` — documentado como bring-up.
- Deveria ser marcado `// BRING-UP ONLY` (hoje só neste README, 0 ocorrências
  no código): `polaris_soft_reset` com `0xFFFFFFFF`, `wait_idle` com `== 0`,
  bases fixas `0xFFFF8001...`/`0xFFFF8002...` (colide com janela MSI-X em
  `kernel/pci/pci.c`), VRAM placeholder em `agnus_drv.c` (`8GB/512MB`
  sobrescrito por `256MB/64MB`), `release` sem `return`, `chip_family` extraído
  de ponteiro. Não copiar para o driver final sem validar BAR64, WC, IOMMU e
  `CONFIG_MEMSIZE`.
- Headers duplicados para unificar um dia: `kernel/include/agnus_drv.h` vs
  `agnus/agnus_drv.h`, `kernel/include/polaris.h` vs `agnus/polaris.h`.