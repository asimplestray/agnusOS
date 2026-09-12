# AgnusOS: plano completo para deixar de ser um hobby OS

## Propósito

Este documento reúne os requisitos técnicos, operacionais e de engenharia necessários para transformar o AgnusOS em um sistema operacional confiável, estável, seguro, documentado e sustentável. Ele não pressupõe que funcionalidades parcialmente implementadas estejam prontas e não trata compilação bem-sucedida como prova de correção.

O documento deve permanecer vivo. Cada item concluído precisa apontar para testes automatizados, documentação, commits e evidências reproduzíveis. Um item não deve ser marcado como concluído apenas porque existe algum código relacionado.

## Definição de “não ser mais um hobby OS”

O AgnusOS somente deve ser considerado pronto para uso sério quando conseguir demonstrar, de forma repetível:

- isolamento efetivo entre kernel, processos e dispositivos;
- ausência de corrupção de memória nos testes suportados;
- gerenciamento previsível de recursos e erros;
- ABI pública versionada, coerente e testada;
- atualizações e releases reproduzíveis;
- boot, instalação, recuperação e diagnóstico documentados;
- compatibilidade de hardware explicitamente delimitada;
- testes contínuos em emuladores e máquinas reais;
- política de segurança e processo de resposta a vulnerabilidades;
- estabilidade suficiente para executar cargas reais por períodos prolongados;
- manutenção possível por contribuidores que não sejam o autor original.

## Escala de maturidade

- **M0, protótipo:** inicializa, imprime mensagens e executa caminhos felizes.
- **M1, experimental:** possui subsistemas funcionais, mas sem garantias de isolamento ou compatibilidade.
- **M2, desenvolvimento:** contratos internos definidos, testes automatizados básicos e falhas conhecidas rastreadas.
- **M3, alpha:** isolamento básico, gerenciamento de recursos, ABI candidata e boot automatizado.
- **M4, beta:** workloads reais, atualização, recuperação, testes de estresse e hardware suportado documentado.
- **M5, produção limitada:** segurança revisada, releases reproduzíveis, telemetria local, suporte e regressões controladas.
- **M6, produção geral:** histórico de estabilidade, ecossistema utilizável, governança madura e manutenção sustentável.

O estado atual deve ser tratado como M1 até que os bloqueadores críticos das primeiras fases sejam resolvidos e comprovados por testes.

## Regras para concluir itens

Para cada requisito abaixo, registrar:

1. responsável técnico;
2. issue ou conjunto de issues;
3. projeto da solução e invariantes;
4. testes unitários, integração, estresse e negativos aplicáveis;
5. resultado em QEMU e, quando relevante, hardware real;
6. documentação pública e interna;
7. impacto na ABI e estratégia de compatibilidade;
8. critérios objetivos de aceite;
9. plano de rollback ou recuperação;
10. métricas antes e depois da mudança.

## Legenda de acompanhamento

- `[ ]`: ainda não implementado ou não comprovado;
- `[~]`: implementação parcial, ainda sem todos os testes ou invariantes;
- `[x]`: concluído e acompanhado de evidência reproduzível;
- **Bloqueador:** impede executar aplicações não confiáveis ou produzir release;
- **Evidência:** arquivo, teste ou comando que demonstra o estado descrito.

## Estado resumido em setembro de 2026

O kernel possui uma base ampla, incluindo memória virtual, tarefas, VFS,
FAT32, rede, DRM/GEM, IPC, ABI pública e aplicações em ring 3. O foco imediato
deve ser consolidar isolamento, ownership, teardown, tratamento de erros e
testes automatizados, em vez de adicionar novos subsistemas.

Ordem recomendada de execução:

1. concluir e auditar a fronteira userspace/kernel;
2. corrigir lifecycle de tarefas, arquivos, sockets e objetos compartilhados;
3. fortalecer PMM, VMM, heap, page faults e isolamento;
4. automatizar boot e testes negativos em QEMU;
5. estabilizar concorrência, interrupções, VFS e armazenamento;
6. eliminar warnings e adicionar configurações de diagnóstico;
7. somente então ampliar GUI, drivers e suporte a hardware.

---

# P0: bloqueadores absolutos de segurança e correção

Nenhum release que execute aplicações não confiáveis deve ocorrer enquanto esta seção estiver incompleta.

## 1. Fronteira entre userspace e kernel

- [x] Definir o intervalo virtual permitido para userspace em x86-64.
  Evidência: `VMM_USER_MIN` e `VMM_USER_MAX` em `kernel/include/vmm.h`.
- [x] Rejeitar endereços fora da metade canônica inferior reservada ao userspace.
  Evidência: validação de início e fim em `user_range_valid()`.
- [x] Rejeitar ranges que sofram overflow em `base + size`.
  Evidência: verificação explícita contra `UINT64_MAX` em `kernel/mem/uaccess.c`.
- [x] Implementar `access_ok()` para leitura e escrita.
  Evidência: validação por página dos bits `PRESENT`, `USER` e `WRITE`.
- [x] Implementar `copy_from_user()` com retorno de falha controlada.
- [x] Implementar `copy_to_user()` com retorno de falha controlada.
- [x] Implementar cópia limitada de strings vindas de userspace.
  Evidência: `strncpy_from_user()` exige capacidade e terminação NUL.
- [x] Tratar buffers que atravessam páginas válidas e inválidas.
  Evidência: `copy_user_pages()` divide a operação por fronteiras de 4 KiB.
- [ ] Impedir a desreferência direta de ponteiros userspace no kernel.
- [ ] Auditar todas as syscalls, ioctls, sockets, VFS, IPC e drivers.
- [ ] Definir comportamento para cópias parciais.
- [ ] Converter page faults durante user-copy em erros como `-EFAULT`, não em panic.
- [ ] Testar ponteiros nulos, kernel pointers, páginas ausentes e ranges enormes.
- [ ] Testar mudanças concorrentes de mappings durante chamadas ao kernel.
- [~] Documentar quais funções aceitam ponteiros userspace e quais aceitam apenas kernel pointers.
  Estado: o contrato geral existe em `kernel/include/uaccess.h`, mas ainda falta
  classificar individualmente todas as interfaces públicas.

### Próxima entrega executável

1. criar inventário de todos os argumentos ponteiro das 66 traps;
2. marcar cada argumento como entrada, saída, entrada/saída ou kernel-only;
3. substituir acessos diretos por buffers locais e helpers de uaccess;
4. padronizar falha de endereço como `AOS_ERR_BAD_ADDRESS` ou equivalente;
5. criar aplicação ring 3 que teste uma página válida seguida de página ausente;
6. executar o teste para cada syscall que aceite ponteiros.

### Critério de aceite

Uma suíte ring 3 deve enviar ponteiros inválidos e buffers em fronteiras de página para todas as interfaces públicas sem provocar panic, corrupção, leak ou acesso a memória do kernel.

## 2. Address spaces, VMA e page faults

- [ ] Criar representação de regiões virtuais por processo.
- [ ] Registrar início, fim, proteção, tipo, offset e objeto de backing de cada VMA.
- [ ] Proteger a estrutura de VMAs com locking e regras de lifetime.
- [~] Rejeitar faults fora de uma VMA autorizada.
  Enquanto não há uma estrutura geral de VMA, o handler deixou de criar páginas
  anônimas para endereços userspace arbitrários. Somente crescimento controlado
  da stack é aceito; os demais faults ausentes encerram a tarefa.
- [~] Validar permissões de leitura, escrita e execução em cada fault.
  Estado: existem bits arquiteturais `PF_ERR_W`, `PF_ERR_U` e `PF_ERR_I`, mas
  o handler ainda não os confronta com uma política de VMA.
- [~] Implementar páginas anônimas zero-filled.
  Evidência: `handle_demand_page_fault()` aloca e zera páginas. Bloqueador:
  atualmente pode fazê-lo sem confirmar que o endereço pertence a uma VMA.
- [ ] Implementar mappings de arquivos com offsets e tamanho correto.
- [~] Implementar crescimento de stack limitado e com guard pages.
  `handle_stack_growth()` limita a região a 1 MiB abaixo de `start_stack`, mantém
  a página do limite inferior não mapeada, exige proximidade de até 64 KiB ao RSP
  da frame e cria páginas writable e NX. Ainda faltam VMA formal e testes Ring 3.
- [ ] Definir política de overcommit.
- [ ] Implementar `mmap`, `munmap` e alteração de proteção, se fizerem parte da ABI.
- [ ] Dividir e mesclar VMAs corretamente.
- [~] Invalidar TLB após alterações de mappings.
  Evidência: `vmm_unmap_page_in_pml4()` executa `invlpg` quando altera o PML4
  ativo. Ainda falta garantir invalidação em todos os caminhos de mudança.
- [ ] Preparar shootdown de TLB para SMP.
- [ ] Impedir mappings userspace sobre páginas do kernel, MMIO reservado ou page tables.
- [ ] Definir política W^X e evitar páginas simultaneamente graváveis e executáveis.
- [ ] Implementar ASLR quando a base de memória estiver correta.
- [ ] Implementar copy-on-write para fork ou definir explicitamente a ausência de fork.
- [~] Garantir cleanup integral no exit e em falhas parciais de exec.
  Estado: `task_exit()` reduz o refcount de `mm` e chama `vmm_free_pml4()` na
  última referência, mas o teardown ocorre antes da troca segura de CR3 e não
  abrange todos os recursos possuídos pela tarefa.
- [ ] Testar OOM em cada nível de criação de page table.
- [ ] Fazer rollback de mappings incompletos.

## 3. Separação kernel e userspace

- [ ] Revisar o layout completo do PML4.
- [ ] Garantir que mappings do kernel tenham bit supervisor-only.
- [ ] Não compartilhar entradas de metade baixa que exponham heap ou dados do kernel.
- [ ] Definir uma região superior estável para kernel, direct map, MMIO e stacks.
- [ ] Mapear apenas o necessário em cada address space.
- [ ] Revisar permissões NX, writable e global de todas as regiões.
- [ ] Proteger páginas de texto do kernel contra escrita.
- [ ] Proteger dados read-only após inicialização.
- [ ] Remover identidade de mappings temporários após o boot quando possível.
- [ ] Auditar acesso a memória física baixa, ACPI e firmware.
- [ ] Adicionar testes que tentem ler e escrever endereços do kernel em ring 3.

## 4. Context switch e entrada/saída de syscalls

- [ ] Especificar em documento a ABI exata de traps e syscalls.
- [ ] Documentar registradores de número, argumentos, retorno e clobbers.
- [ ] Validar todos os argumentos com padrões distintos em teste ring 3.
- [ ] Garantir alinhamento de stack exigido pela ABI C.
- [ ] Preservar todos os registradores obrigatórios.
- [ ] Validar frames de interrupção com e sem mudança de privilégio.
- [ ] Tratar corretamente retorno para endereços inválidos.
- [ ] Revisar flags restauráveis pelo userspace.
- [ ] Planejar estado per-CPU e tratamento de GS para SMP.
- [~] Mover a troca de CR3 para um ponto comprovadamente seguro.
  Estado: `schedule()` em `kernel/task.c:369-378` ativa o PML4 do próximo antes da
  troca de stack (não depois de `context_switch()`). Evita escrever CR3 quando já é
  o atual, mas ainda precisa de revisão formal das stacks e mappings de transição.
- [ ] Garantir que stacks de transição existam nos dois address spaces necessários.
- [ ] Gerar offsets de estruturas usados pelo assembly.
- [ ] Adicionar `_Static_assert` para offsets ainda hardcoded.
- [ ] Testar syscalls interrompidas, aninhadas e executadas durante preempção.
- [ ] Fazer fuzzing do dispatcher de syscalls.

## 5. PMM, heap e allocators

- [~] Proteger o bitmap e contadores do PMM contra concorrência.
  Evidência: `pmm_alloc_block()` usa spinlock com irqsave. Ainda é necessário
  auditar todos os caminhos de reserva, liberação e atualização de contadores.
- [ ] Tornar alocações e liberações seguras em contextos IRQ quando permitido.
- [ ] Detectar double-free no PMM em builds de debug.
  Estado: `pmm_free_block()` rejeita endereço zero e desalinhado, e
  `bitmap_clear()` evita incrementar `free_pages` quando o bit já está livre.
  Entretanto, a operação falha silenciosamente e não distingue double-free de
  endereço inválido; falta diagnóstico em debug e teste de regressão.
- [ ] Detectar páginas reservadas liberadas indevidamente.
  Risco: depois da inicialização, o PMM não mantém uma classificação separada
  para páginas permanentemente reservadas. Um endereço alinhado pertencente ao
  kernel, bitmap, Multiboot ou módulo pode ser passado a `pmm_free_block()` e
  ficar disponível novamente.
- [ ] Validar alinhamento e limites de todas as regiões físicas.
  Estado: endereços fora de `total_pages` são tratados como ocupados nos helpers
  do bitmap e frees desalinhados são rejeitados. Ainda faltam validação de
  overflow em `entry->addr + entry->len`, arredondamento seguro e verificação
  estrutural completa das entradas Multiboot2.
- [ ] Tratar corretamente mapas de memória fragmentados.
  Estado: regiões `MULTIBOOT_MEMORY_AVAILABLE` são liberadas e regiões críticas
  conhecidas são reservadas novamente. Falta testar regiões sobrepostas, não
  ordenadas, truncadas e acima do limite endereçável pelo kernel.
- [ ] Separar memória normal, DMA32, DMA e regiões reservadas.
- [~] Proteger o heap global com locking apropriado.
  Estado: o allocator usa locking, mas faltam testes concorrentes, regras para
  contexto IRQ e detecção de corrupção e double-free.
- [ ] Detectar double-free, UAF e corrupção de metadados em debug.
  Estado: `kfree()` ignora ponteiros fora do heap e blocos já livres. Isso reduz
  dano em alguns casos, mas esconde erros e não valida se o ponteiro aponta para
  o início exato de uma alocação. Um ponteiro interior pode fazer o allocator
  interpretar dados comuns como metadados.
- [ ] Adicionar red zones, poisoning e canários em builds de diagnóstico.
- [ ] Definir variantes de alocação que podem dormir e variantes atômicas.
- [ ] Garantir que `realloc`, se existir, trate overflow e preserve conteúdo.
  Estado parcial: `krealloc()` trata `NULL`, tamanho zero, overflow do
  alinhamento e preserva o conteúdo antigo ao mover a alocação. Ainda precisa
  alinhar o novo tamanho antes do split, validar o ponteiro antes de acessar seu
  header e possuir testes de crescimento, redução e falha.
- [ ] Usar helpers de multiplicação segura para arrays.
- [ ] Criar testes de fragmentação e coalescência.
  Evidência parcial: o heap implementa first-fit, split e coalescência de blocos
  livres adjacentes. Não há teste automatizado das invariantes da lista após
  sequências adversariais de alloc, free e realloc.
- [ ] Executar testes concorrentes de alocação.
- [ ] Injetar falhas determinísticas em cada ponto de alocação.
- [ ] Expor estatísticas de uso e leaks.
- [ ] Considerar slab/slub ou caches de objetos após a correção do allocator base.

## 6. Modelo de processos e encerramento

- [ ] Definir estados formais de processo e thread.
- [ ] Separar processo, thread, credenciais, arquivos e address space.
- [~] Implementar referências para objetos compartilhados.
  Estado: `mm_struct` possui refcount e tarefas filhas incrementam a referência.
  Ainda faltam helpers atômicos, invariantes documentadas e cobertura dos
  demais objetos compartilhados.
- [x] Não liberar o address space atual antes de trocar para um contexto seguro.
  Evidência: `task_exit()` em `kernel/task.c:283-288` faz `vmm_activate_pml4(kernel_pml4_phys)`
  antes de `vmm_free_pml4()`. Ainda falta inventário central de recursos + testes de teardown concorrente (ver itens abaixo).
- [ ] Implementar zombie, wait e reap, ou um modelo alternativo documentado.
- [ ] Liberar kernel stack após a thread deixar de executá-lo.
- [ ] Fechar descritores no exit.
- [ ] Liberar sockets, ports, pipes, locks e recursos DRM no exit.
  Estado: existem rotinas locais de cleanup em alguns subsistemas, mas não há um
  inventário central de recursos pertencentes à tarefa. Message ports usam IDs
  globais sem owner, e pipes dependem do fechamento correto das duas pontas.
- [ ] Reassociar ou encerrar filhos conforme a política definida.
- [ ] Implementar limites de processos e recursos.
  Risco: IDs de message ports e tokens de reply crescem globalmente sem quota,
  e cada mensagem consome heap do kernel. Faltam limites por tarefa, limite
  global, tratamento de wraparound e backpressure documentada.
- [~] Definir sinais ou mecanismo equivalente com semântica consistente.
  Estado: tarefas possuem bitmasks `sig_recv`, `sig_wait` e `sig_except`, além
  das syscalls de sinal. Faltam atomicidade, permissões, wakeups completos e
  documentação da semântica.
- [ ] Implementar terminação segura causada por faults userspace.
- [ ] Garantir rollback completo quando criação, load ou exec falhar.
- [~] Auditar todos os contadores de referência do `mm`.
  Estado observado: criação de tarefa incrementa diretamente `mm->refcount` e
  exit decrementa sob o lock da runqueue. Faltam helpers dedicados, proteção
  contra underflow e testes de criação/saída concorrentes.
- [ ] Testar criação e encerramento repetidos até milhões de ciclos em ambiente automatizado.

## 7. Preempção, locking e concorrência

- [ ] Fazer o scheduler corresponder ao comportamento documentado.
- [~] Implementar preempção real ou declarar e testar um kernel cooperativo.
  Estado: há scheduler round-robin, `need_resched`, timer e locks da runqueue;
  falta provar preempção, estados de bloqueio e comportamento sob carga.
- [ ] Definir regiões não preemptíveis.
- [~] Definir semântica dos spinlocks com interrupções.
  Evidência: diversos subsistemas usam `spin_lock_irqsave()` e
  `spin_unlock_irqrestore()`. Falta documentar contextos permitidos, nesting e
  hierarquia global de locks.
- [ ] Adicionar lockdep simplificado ou verificação de ordem de locks em debug.
- [ ] Documentar hierarquia de locks por subsistema.
- [ ] Eliminar espera bloqueante em contexto IRQ.
- [ ] Garantir que callbacks de interrupção não usem operações inseguras.
- [~] Revisar estado global mutável sem sincronização.
  Estado: PMM, heap, runqueue, VFS, IPC, rede e DRM já possuem locks em partes
  dos caminhos, mas a auditoria global ainda não foi concluída.
- [ ] Preparar estruturas per-CPU.
- [ ] Implementar atomics com barreiras adequadas à arquitetura.
- [ ] Usar acquire/release onde necessário.
- [ ] Testar condições de corrida com alta frequência de timer e I/O.
- [ ] Criar watchdog para detectar hard lockups e stalls.
- [ ] Medir latência máxima de interrupção e scheduling.

## 8. Interrupções, exceções e MSI

- [x] Definir um único responsável por enviar EOI em cada caminho.
  Evidência: `irq_dispatcher()` em `kernel/cpu/idt.c:400-420` não envia EOI; único EOI em
  `interrupt_handler()` (`:454-461`) para vetores 32-47. Risco antigo de duplo EOI eliminado.
- [x] Eliminar possibilidade de EOI duplicado.
  Ação feita: EOI só no dispatcher comum. Falta adicionar contadores de regressão para IRQ master/slave.
- [ ] Substituir aritmética baseada no tamanho dos stubs MSI por tabela de símbolos.
  Urgente: stubs MSI têm 12 bytes (`interrupts.asm:150-158`) mas `idt.c:171-173` avança `*13` — vetores ≥49 dessincronizam.
- [x] Corrigir comparações impossíveis e tipos de vetor.
  Como vetores MSI usam `uint8_t`, comparações com valores acima de 255 eram
  sempre falsas. A validação agora verifica somente o limite inferior 48; o
  limite superior é garantido pelo próprio tipo.
- [ ] Validar limites da IDT e presença de handlers.
- [~] Implementar registro compartilhado de handlers MSI.
  Evidência: `request_msi_irq()` mantém uma lista de ações por vetor, suporta
  handlers diretos e threaded e instala `msi_dispatcher` na tabela global.
- [x] Tornar `free_msi_irq()` seguro contra trabalho threaded pendente.
  A ação é primeiro removida da lista, o teardown aguarda dispatchers que já
  possuíam referência e `cancel_work_sync()` remove trabalho pendente ou espera
  a execução terminar antes de liberar a memória.
- [~] Implementar allocator de vetores MSI-X.
  Evidência: `pci_msix_alloc_vectors()` reserva ranges contíguos entre os
  vetores 48 e 255 sob spinlock.
- [ ] Corrigir contrato da liberação de vetores MSI-X.
  Risco: a alocação retorna apenas o vetor-base, enquanto a liberação recebe um
  array de vetores. O chamador precisa reconstruir corretamente o range, e não
  existe owner por dispositivo para impedir double-free ou liberação alheia.
- [ ] Configurar e validar capability MSI/MSI-X no dispositivo PCI.

### Auditoria aprofundada de exceções e page faults

- [x] Corrigir a ABI entre o stub assembly de page fault e `vmm_page_fault_handler()`.
  Evidência: assinatura atual `vmm_page_fault_handler(struct interrupt_frame*)` (`kernel/include/vmm.h:58`,
  registrada em `kernel/cpu/idt.c:187`), stub genérico `ISR_ERRCODE 14` (`kernel/cpu/interrupts.asm:112`),
  frame canônico (`kernel/include/idt.h:26-41`), CR2 lido no C + `frame->err_code` (`kernel/mem/vmm.c:299-313`).
  Contrato antigo `RDI/RSI/RDX` não existe mais.
- [x] Corrigir o layout empilhado pelo stub de page fault.
  Evidência: sem `isr14` customizado; caminho comum + `struct interrupt_frame` única. Falta validar offsets com asserts de build.
- [x] Fazer o retorno do stub de page fault consumir exatamente os campos extras.
  Evidência: sem campo extra de CR2 na pilha; retorno via caminho comum. Manter auditoria de `iretq` nos testes dos 32 vetores.
- [x] Não mapear automaticamente qualquer endereço userspace ausente.
  O handler não realiza mais demand paging genérico. Na ausência de uma VMA
  geral, somente crescimento de stack validado é recuperável; qualquer outro
  acesso ausente em Ring 3 termina somente a tarefa causadora.
- [ ] Exigir uma VMA válida antes de realizar demand paging.
  A VMA deve conter o endereço, permitir a operação indicada pelo error code e
  definir backing anônimo ou de arquivo. Ausência de VMA precisa resultar em
  terminação apenas da tarefa, sem criar página e sem panic global.
- [~] Interpretar todos os bits relevantes do error code de page fault.
  Devem ser tratados explicitamente present, write/read, user/supervisor,
  reserved-bit, instruction-fetch e protection-key quando aplicável. Fault por
  reserved bit, protection violation, instruction fetch e write/read já são
  classificados. Ainda falta suporte explícito a protection keys.
- [x] Propagar OOM de demand paging para a tarefa causadora.
  Falha de alocação ou mapping faz o crescimento da stack falhar e converge para
  `task_exit(-14)`, evitando repetição indefinida da instrução.
- [x] Verificar a falha de `vmm_map_page_in_pml4()` e liberar a página física.
  O mapping é conferido com `vmm_get_phys()`; se a página não estiver instalada,
  o frame físico é liberado e o fault é tratado como fatal para a tarefa.
- [ ] Aplicar as permissões reais da VMA ao mapping criado sob demanda.
  O caminho atual constrói mappings de usuário e só adiciona write/NX conforme
  flags recebidas, mas os callers passam flags insuficientes. O mapping final
  deve derivar permissões de leitura, escrita e execução da VMA, com W^X quando
  essa política for adotada.
- [~] Limitar crescimento da stack usando limites inferior e superior formais.
  O limite superior é `start_stack`; o inferior é 1 MiB abaixo, com uma página
  reservada como guarda e validação de proximidade ao RSP. Falta representar
  esses limites em uma VMA formal e adicionar testes.
- [~] Manter uma guard page não mapeada na stack de usuário.
  O handler recusa a página no limite inferior da janela de stack. Falta tornar
  a guard page parte explícita da futura VMA e validar o comportamento em Ring 3.
- [x] Terminar a tarefa em faults Ring 3 de proteção e ausência inválida.
  Violações de proteção, reserved-bit faults, instruction fetch em página
  ausente e ausências fora do crescimento de stack convergem para
  `task_exit(-14)` sem panic global.
- [x] Garantir que exceções Ring 3 sem handler registrado evitem panic global.
  `interrupt_handler()` agora identifica CPL 3 pelos bits baixos de CS e chama
  `task_exit()` para encerrar apenas a tarefa causadora. Exceções Ring 0 sem
  handler continuam no caminho de dump e `PANIC`.
- [ ] Classificar exceções fatais de kernel, recuperáveis de kernel e atribuíveis
  a userspace.
  Double fault, machine check e corrupção interna exigem política distinta de
  `#DE`, `#UD`, `#GP`, `#AC` ou `#XM` originados em Ring 3.
- [x] Validar CPL usando os dois bits baixos de CS, sem depender apenas de
  valores literais.
  A origem userspace agora é detectada com `(frame->cs & 3) == 3` tanto para a
  política de exceções quanto antes de `do_signal()`.
- [x] Evitar executar `do_signal()` depois de uma exceção fatal de Ring 3.
  Exceções sem handler chamam diretamente `task_exit()`, declarado `noreturn`,
  antes do caminho de sinais. `task_exit()` suspende a tarefa e força uma troca
  de contexto; se o scheduler retornar sem trocar, o kernel entra em `PANIC` em
  vez de retornar por `iretq` para o contexto fatal.
- [ ] Produzir diagnóstico estruturado de kernel faults.
  O caminho de page fault de kernel atualmente imprime textos incompletos e
  entra diretamente em `hlt`, sem usar o mecanismo normal de panic, sem CR2,
  error code decodificado, RIP confiável ou backtrace padronizado.
- [ ] Testar individualmente os 32 vetores de exceção suportados.
  Os testes devem diferenciar origem Ring 0/Ring 3, presença de error code,
  preservação dos registradores, alinhamento da pilha e retorno ou terminação
  esperados.
- [ ] Adicionar regressões Ring 3 para acesso nulo, endereço não canônico, NX,
  escrita read-only, divisão por zero e opcode inválido.
  O critério de aceite é encerrar somente o processo causador, manter o kernel e
  outras tarefas vivos e não aumentar contadores de páginas após o reap.

### Auditoria aprofundada de IRQ, PIC e MSI/MSI-X

- [x] Proibir liberação de `irq_action` enquanto o dispatcher percorre a lista.
  `irq_dispatcher()` registra travessias ativas sob `irq_lock`; `free_irq()`
  remove a ação e aguarda todas as travessias anteriores terminarem antes de
  cancelar trabalho threaded e liberar a ação.
- [x] Proibir liberação de ação MSI enquanto o dispatcher percorre a lista.
  `msi_dispatcher()` e `free_msi_irq()` usam a mesma política de travessias
  ativas por vetor, impedindo liberação enquanto uma lista antiga ainda é usada.
- [x] Tornar trabalho threaded idempotente ou rejeitar enfileiramento duplicado.
  `work_struct` agora possui estados pending e running. `queue_work()` rejeita
  uma segunda inserção enquanto o item já está pendente ou executando, evitando
  corrupção do ponteiro intrusivo `next`.
- [ ] Preservar um frame ou contexto válido para threaded IRQ.
  `irq_thread_work()` entrega ao handler uma frame local totalmente zerada.
  Handlers que dependam de vetor, estado ou metadados da interrupção recebem
  dados fictícios. O contrato deve proibir esse uso ou capturar os campos
  necessários antes de sair do hard IRQ.
- [~] Não tratar EOI de MSI/MSI-X como EOI de PIC.
  O EOI incorreto enviado ao PIC por `msi_dispatcher()` foi removido. Ainda é
  necessário integrar o EOI do APIC local quando o controlador APIC estiver
  efetivamente habilitado.
- [ ] Validar spurious IRQ7/IRQ15 antes de enviar EOI.
  O PIC possui regras específicas para interrupções espúrias. O dispatcher deve
  consultar ISR e evitar reconhecer uma IRQ inexistente de forma incorreta.
- [ ] Mascarar a linha antes de remover seu último handler.
  A ordem de teardown deve impedir uma nova interrupção entre a remoção da lista
  e o mascaramento físico.
- [x] Cancelar e drenar threaded IRQ antes de liberar `irq_action`.
  `free_irq()` e `free_msi_irq()` aguardam dispatchers anteriores e chamam
  `cancel_work_sync()` antes de liberar o `work_struct` embutido na ação.
- [ ] Definir compartilhamento de IRQ por flags e validar `dev_id`.
  Atualmente múltiplas ações são sempre anexadas à mesma linha, sem política de
  compartilhamento, verificação de compatibilidade ou rejeição de `dev_id`
  duplicado.
- [ ] Contabilizar IRQs não tratadas e mascarar storms.
  Linhas sem dispositivo reivindicando a interrupção precisam de limiar,
  diagnóstico e mitigação para não monopolizar CPU.
- [ ] Validar a capability list PCI contra ciclos e offsets inválidos.
  `pci_find_capability()` segue ponteiros fornecidos pelo dispositivo sem limite
  de iterações ou bitmap de visitados. Uma lista cíclica pode travar o kernel.
- [ ] Validar o bit de capability list no status PCI antes de seguir `cap_ptr`.
- [ ] Validar alinhamento e faixa de cada capability PCI.
  Ponteiros devem ser alinhados, permanecer no config space suportado e deixar
  espaço suficiente para a estrutura acessada.
- [ ] Adicionar ownership ao allocator de vetores MSI-X.
  Cada range deve pertencer a um dispositivo ou objeto IRQ, impedindo double
  free, liberação cruzada e vazamento no rollback de inicialização.
- [ ] Testar IRQ compartilhada, storm, remoção concorrente e threaded handler
  pendente.

  O allocator atual reserva números no kernel, mas isso não comprova que tabela
  MSI-X, mask bits, message address/data e teardown do dispositivo tenham sido
  programados corretamente.
- [ ] Implementar handlers seguros para todas as exceções arquiteturais.
- [x] Diferenciar faults de kernel e userspace.
  Evidência: `interrupt_handler()` em `kernel/cpu/idt.c:424,431-435` faz `(frame->cs & 3)==3` → `task_exit()`; `kernel/mem/vmm.c:319-344` converge para `task_exit(-14)`. Exceções Ring 0 sem handler seguem para dump/`PANIC`.
- [x] Encerrar apenas o processo em faults recuperáveis de ring 3.
  Evidência: mesma acima — sem panic global para Ring 3.
- [ ] Implementar double-fault stack via IST.
- [ ] Criar stacks IST para NMI e machine check quando aplicável.
- [ ] Proteger contra stack overflow no kernel.
- [ ] Migrar de PIC para APIC/IOAPIC como caminho principal moderno.
- [ ] Implementar roteamento ACPI de IRQs.
- [ ] Validar MSI e MSI-X por dispositivo.
- [ ] Implementar afinidade de IRQ para SMP.
- [ ] Adicionar contadores e diagnóstico por vetor.

---

# P1: arquitetura fundamental necessária para estabilidade

## 9. VFS, arquivos por abertura e descritores por processo

- [~] Separar inode/node persistente de uma descrição de arquivo aberta.
  Evidência: `vfs_node_t` e `file_t` são tipos distintos. Falta garantir que
  todas as APIs DOS, devices e filesystems usem a separação consistentemente.
- [x] Criar `struct file` ou equivalente por abertura.
  Evidência: `file_t` é alocado por `vfs_file_open()` e liberado por
  `vfs_file_close()`.
- [x] Armazenar posição, flags, operações e `private_data` por abertura.
  Evidência: esses campos existem em `file_t`; a posição possui spinlock.
- [ ] Criar tabela de descritores por processo.
  Estado: o VFS já representa cada abertura com `file_t`, mas não existe uma
  tabela genérica, única e pertencente à tarefa para arquivos, pipes e devices.
  As camadas atuais ainda mantêm handles e objetos por mecanismos paralelos.
- [ ] Proteger tabela de descritores contra concorrência.
- [ ] Implementar duplicação de descritores, se exposta pela ABI.
- [ ] Implementar close-on-exec.
- [ ] Impedir que um processo use descritores de outro.
- [~] Definir ownership e lifetime de dentries, mounts, nodes e files.
  Estado: `file_t` possui criação e fechamento explícitos, incluindo callback
  `release`; nodes e mounts ainda não possuem refcount ou contrato completo.
- [~] Implementar propagação precisa de erros.
  Estado: operações de `file_t` propagam retornos dos callbacks, mas helpers
  antigos como `vfs_read()` e `vfs_write()` ainda retornam zero quando a
  operação não existe, confundindo ausência de suporte com sucesso/EOF.
- [ ] Eliminar buffers estáticos não reentrantes.
- [ ] Remover cópias de string sem limites.
- [ ] Normalizar paths com tratamento seguro de `.` e `..`.
- [ ] Aplicar permissões após resolução de symlinks e mount boundaries.
- [ ] Prevenir loops de symlinks.
- [x] Definir tamanho máximo de path e nome.
  Evidência: `VFS_MAX_PATH` é 4096 e `VFS_MAX_NAME` é 128 em
  `kernel/include/vfs.h`.
- [ ] Tratar operações atômicas de rename e unlink.
- [ ] Implementar page cache ou documentar limitações de I/O síncrono.
- [ ] Definir sincronização entre leitura, escrita, truncate e mmap.
- [ ] Criar testes de concorrência para open, close, rename e unlink.

### Pipes

- [~] Implementar pipe com buffer circular, espera e notificação.
  Evidência: `kernel/fs/pipe.c` possui buffer de 4 KiB, posições circulares,
  wait queues separadas e wakeup de leitores e escritores.
- [~] Implementar EOF e erro quando a ponta oposta fecha.
  Evidência: leitura retorna EOF quando não existem escritores e escrita falha
  quando não existem leitores. A ABI de erro ainda usa `(uint32_t)-1`, sem erro
  tipado ou integração consistente com a semântica pública.
- [ ] Tratar escrita maior que a capacidade do pipe.
  Risco confirmado: a condição `pipe->size + size > PIPE_BUF_SIZE` nunca pode
  se tornar falsa quando `size` sozinho excede 4 KiB, causando espera
  indefinida mesmo quando o pipe está vazio.
- [ ] Tornar fechamento e lifetime do pipe seguros contra concorrência.
  Risco: `pipe_close()` libera os dois nodes e o estado depois que os contadores
  chegam a zero, sem refcount explícito ou exclusão comprovada de leitores,
  escritores e waiters que ainda conservem ponteiros para o objeto.
- [ ] Impedir underflow dos contadores de leitores e escritores.
- [ ] Testar wraparound, bloqueio, EOF, ponta quebrada, fechamento concorrente e
  operações maiores, menores e iguais a `PIPE_BUF_SIZE`.

### Message ports

- [~] Implementar filas FIFO de mensagens e reply tokens.
  Evidência: `kernel/ipc/msgport.c` mantém filas por port, payload limitado,
  espera, reply port e token de resposta de uso único, com selftest básico.
- [~] Proteger o registry global e filas contra concorrência.
  Evidência: existe `registry_lock` com irqsave e lock por port. A maioria das
  operações depende do lock global para manter o objeto vivo, mas falta uma
  especificação formal da ordem dos locks e testes concorrentes.
- [ ] Associar cada port e reply pendente à tarefa proprietária.
- [ ] Aplicar autorização a put, get, reply e delete.
- [ ] Cancelar waiters de forma segura ao excluir um port.
  Risco: `msgport_delete()` acorda a fila e libera imediatamente o port. É
  necessário provar que nenhum waiter retomará usando a wait queue liberada.
- [ ] Limitar quantidade de ports, mensagens enfileiradas, replies e memória.
- [ ] Detectar wraparound e reutilização insegura de IDs e tokens.
- [ ] Executar testes de criação, exclusão, timeout, reply, flood e teardown de
  tarefa sob concorrência.

## 10. uAPI e compatibilidade binária

- [x] Escolher um nome canônico: `AgnusOS` (único no repo — `kernel/kernel.c`, `README.md`, `grub.cfg`, `Makefile`). `AmigaOS-style` é adjetivo de API, não nome concorrente.
- [x] Definir uma única fonte numérica para syscalls.
  Evidência: `kernel/include/uapi/aos.h:20-89` define de forma canônica as 66 traps 0-65
  e `kernel/include/syscall.h:116` consome esse mapa.
  Ressalva: `AOS_SetProcGroup/GetProcGroup/SetConProc/GetConProc` (28-31) publicados mas sem handler em `kernel/syscall.c:29-92` — retornam `NOT_FOUND` até implementação.
- [~] Eliminar mapas antigos conflitantes.
  Estado: aliases `SYS_*` permanecem para compatibilidade de fonte, mas apontam
  para os números canônicos `AOS_*`. Ainda é necessário procurar consumidores
  externos com números hardcoded antes de remover ou congelar aliases.
- [ ] Congelar números somente após comportamento funcional.
- [~] Documentar calling convention e tipos ABI com tamanhos explícitos.
  Estado: `uapi/aos.h` documenta `int 0x80`, números e estruturas públicas;
  ainda falta documentar registradores de argumentos, retorno e clobbers de
  forma consistente com `kernel/cpu/syscall.asm`.
- [~] Evitar tipos dependentes do compilador em estruturas públicas.
  Evidência: as estruturas auditadas usam `uint32_t`, `int32_t` e `uint64_t`.
  Ainda é necessário remover `size_t` de contratos públicos ou fixar seu ABI.
- [ ] Definir padding e alinhamento explicitamente.
- [ ] Reservar campos para extensões.
- [ ] Adicionar tamanho e versão a estruturas extensíveis.
- [~] Adicionar `_Static_assert` de tamanho, alinhamento e offsets.
  Evidência: `struct aos_msg`, `struct aos_stat` e layouts DRM essenciais já
  possuem asserts. Faltam alinhamento e offsets de todas as estruturas públicas.
- [ ] Testar headers públicos em C e C++.
- [ ] Testar builds 32-bit apenas se compatibilidade for prometida.
- [ ] Definir endianness da ABI.
- [ ] Definir semântica de erro e relação com `errno`.
- [~] Não retornar sucesso em operações stub.
  Risco observado: `FIONBIO` é aceito por `bsdsocket` sem suporte não bloqueante
  real, e caminhos de DMA/IOMMU ainda retornam sucesso com comportamento vazio.
- [~] Retornar `-ENOTTY`, `-ENOSYS` ou `-EOPNOTSUPP` de forma consistente.
  Estado: VFS e DRM já usam alguns códigos convencionais, mas a política não é
  uniforme e existem helpers que retornam zero quando a operação não existe.
- [ ] Criar testes de compatibilidade entre versões consecutivas.
- [ ] Arquivar headers de cada release.
- [ ] Manter changelog específico de ABI.
- [ ] Adiar a tag uAPI 1.0 até os testes funcionais passarem.

## 11. Carregador ELF, exec e runtime userspace

- [~] Validar magic, classe, arquitetura, endianness e versões ELF.
  Evidência: `elf_load()` valida magic, ELF64, x86-64 e `ET_EXEC`. Faltam
  endianness, versões ELF, tamanho de entry e consistência dos program headers.
- [~] Validar offsets, tamanhos e overflow de todos os headers.
  Estado: há limite básico para program headers e dados de segmentos, mas somas
  como `e_phoff + i * e_phentsize`, `p_offset + p_filesz` e `vaddr + memsz`
  ainda precisam de helpers de overflow antes das comparações.
- [ ] Rejeitar segmentos sobrepostos perigosos.
- [~] Aplicar permissões ELF corretas e W^X.
  Estado: `PF_W` controla `VMM_FLAG_WRITE`, mas segmentos não graváveis ainda
  não recebem NX conforme `PF_X`, e não há rejeição explícita de W+X.
- [x] Zerar corretamente BSS.
  Evidência: cada página recém-alocada é zerada antes da cópia de `p_filesz`,
  cobrindo a região entre `p_filesz` e `p_memsz`.
- [ ] Validar entry point dentro de mapping executável.
- [ ] Construir stack inicial com alinhamento ABI.
- [ ] Definir `argc`, `argv`, `envp` e auxiliary vector.
- [ ] Implementar executáveis PIE se desejado.
- [~] Implementar linker dinâmico ou declarar suporte apenas estático.
  Estado atual: o loader declara e implementa apenas ELF64 estático com
  segmentos `PT_LOAD`; falta transformar essa limitação em contrato de ABI.
- [ ] Definir TLS para threads e bibliotecas.
- [ ] Fornecer libc mínima ou portar uma libc mantida.
- [ ] Definir startup objects, headers, linker scripts e toolchain userspace.
- [ ] Criar sysroot versionado.
- [ ] Testar binários malformados e truncados.
- [ ] Fazer fuzzing do loader.
- [ ] Garantir que exec seja transacional e preserve o processo anterior em falhas anteriores ao commit.

## 12. Tempo, timers e relógios

- [~] Definir clock monotônico e clock de tempo civil.
  Evidência: `timer_get_ticks()` fornece ticks desde o boot e o RTC fornece epoch.
  Falta separar formalmente as APIs e impedir ajustes civis de afetarem timeouts.
- [ ] Calibrar timers com fontes confiáveis.
- [ ] Suportar HPET, APIC timer ou fontes modernas conforme hardware.
- [ ] Tratar wraparound e overflow.
- [~] Implementar sleeps sem busy-wait onde possível.
  Evidência: `aos_delay()` bloqueia a tarefa, registra timer e chama `schedule()`.
  Faltam interrupção, cancelamento e testes contra wakeup perdido.
- [~] Implementar timers por processo e kernel timers.
  Evidência: `timer_entry_t`, `timer_add()` e `timer_remove()` formam uma fila
  ordenada global. Falta ownership, estado formal e teardown por tarefa.
- [~] Garantir cancelamento seguro de timers concorrentes.
  Estado: inserção e remoção usam spinlock com irqsave. Ainda há risco de lifetime
  porque callbacks recebem ponteiros externos e o timer não registra estado.
- [x] Eliminar use-after-return no timer periódico do timerwheel.
  `timerwheel_register_timer()` deixou de registrar um `timer_entry_t` local na
  stack. O timer periódico agora usa armazenamento estático, preservando seu
  lifetime até a remoção e o rearmamento executados pelo callback.
- [ ] Definir precisão e resolução documentadas.
- [ ] Implementar timezone em userspace, mantendo kernel em UTC.
- [ ] Tratar RTC inválido e ausência de bateria.
- [ ] Preparar sincronização por rede em userspace.
- [ ] Testar saltos de tempo sem quebrar timeouts monotônicos.

## 13. DMA, IOMMU e segurança de dispositivos

- [ ] Corrigir alocação coherent para respeitar tamanhos maiores que uma página.
  Risco confirmado: `default_alloc_coherent()` aloca exatamente um bloco físico
  independentemente de `size`, podendo causar overflow para buffers acima de 4 KiB.
- [~] Implementar máscaras DMA e restrições de endereço.
  Estado: a API existe, mas `default_set_dma_mask()` ignora a máscara e retorna
  sucesso. Nenhuma restrição do dispositivo é aplicada à alocação.
- [~] Separar API coherent e streaming.
  Estado: existem operações distintas de alloc/free, map/unmap e sync, mas os
  caminhos streaming são majoritariamente identity/no-op e não cumprem o contrato.
- [~] Implementar scatter-gather.
  Evidência: existem `scatterlist`, alocação de chain e `map_sg`. Bloqueadores:
  endereço DMA é truncado para 32 bits e não há validação de offsets ou máscara.
- [ ] Aplicar alinhamento e atributos de cache corretos.
- [ ] Sincronizar buffers para CPU e dispositivo quando necessário.
- [~] Implementar domínios IOMMU reais.
  Estado: existe backend VT-d e operações map/unmap, mas attach/detach são stubs
  e não há domínio isolado por dispositivo.
- [ ] Não usar identidade global como configuração de produção.
  Risco confirmado: o caminho atual anuncia e utiliza identity mapping, inclusive
  quando VT-d está ativo, o que não contém DMA malicioso ou defeituoso.
- [ ] Autorizar apenas páginas pertencentes ao dispositivo e operação.
- [ ] Desmapear IOVA no teardown e em erros.
- [ ] Invalidar IOTLB corretamente.
- [ ] Tratar faults de IOMMU e identificar o dispositivo causador.
- [ ] Testar dispositivos que fazem DMA após reset ou detach.
- [ ] Criar estratégia segura quando IOMMU não estiver disponível.

---

# P1: DRM, GEM, PRIME e KMS

## 14. Integração de `/dev/dri/cardN`

- [x] Criar contexto DRM privado por abertura do device.
  Evidência: `drm_file_open()` aloca `drm_file_t`, associa-o a
  `file->private_data` e instala operações por arquivo.
- [x] Liberar o contexto DRM pelo callback de fechamento do VFS.
  Evidência: `drm_file_ops.release` aponta para `drm_file_release()`.

- [ ] Fazer cada `open` criar um contexto DRM independente.
- [ ] Armazenar `drm_file` em `file->private_data`.
- [ ] Chamar release DRM em todo close e exit.
- [ ] Proteger e referenciar a lista de clients abertos.
- [ ] Definir minor nodes, card nodes e render nodes.
- [ ] Aplicar política de permissões aos nodes.
- [ ] Separar operações que exigem master das permitidas em render nodes.
- [ ] Garantir cleanup quando o dispositivo é removido.
- [ ] Tratar hot-unplug sem UAF.
- [ ] Testar dois ou mais processos abrindo o mesmo dispositivo.

## 15. GEM

- [x] Manter tabela de handles GEM por abertura DRM.
  Evidência: cada `drm_file_t` possui tabela, capacidade, próximo handle e lock.
- [x] Tomar referência no lookup e liberar referência ao remover handle.
  Evidência: `drm_gem_handle_lookup()` chama `gem_get()` e delete/release chamam
  `gem_put()`.
- [~] Garantir teardown concorrente da tabela de handles.
  Estado: criação, lookup e delete usam lock, mas `drm_gem_handles_release()`
  percorre a tabela sem adquirir o mesmo lock. É necessário impedir operações
  concorrentes durante o fechamento e documentar essa invariante.

- [ ] Mover handles GEM do dispositivo para `drm_file`.
- [ ] Impedir adivinhação ou acesso cross-process a handles.
- [ ] Manter uma referência por handle.
- [ ] Implementar ioctl GEM close.
- [ ] Liberar todos os handles no fechamento do client.
- [ ] Validar tamanho, alinhamento e domínio em criação.
- [ ] Detectar overflow ao alinhar tamanho.
- [ ] Implementar accounting de memória por processo.
- [ ] Implementar limites e reação a OOM.
- [ ] Definir pin/unpin com referências corretas.
- [ ] Implementar eviction real ou não anunciar suporte.
- [ ] Integrar reservation objects e fences.
- [ ] Definir CPU access begin/end.
- [ ] Testar concorrência entre close, mmap, submission e reset.
- [ ] Implementar nomes globais somente se realmente necessários.

## 16. mmap de buffers gráficos

- [ ] Não expor endereços virtuais do kernel ao userspace.
- [ ] Criar offsets ou tokens mmap opacos.
- [ ] Validar offset contra o `drm_file` atual.
- [ ] Criar VMA associada ao objeto GEM.
- [ ] Manter o objeto vivo enquanto houver mappings.
- [ ] Mapear páginas com bit user e permissões corretas.
- [ ] Tratar cacheability de VRAM e GTT.
- [ ] Implementar fault handler quando mapping for paginado.
- [ ] Desmapear no `munmap` e no exit.
- [ ] Invalidar mappings após hot-unplug de forma segura.
- [ ] Testar mapping parcial, concorrente e além do tamanho do BO.

## 17. dma-buf e PRIME

- [ ] Representar dma-buf como descritor real.
- [ ] Implementar export handle-to-fd.
- [ ] Implementar import fd-to-handle.
- [ ] Preservar referências entre processos.
- [ ] Implementar attach, detach, map e unmap por device.
- [ ] Integrar reservation e fences implícitas.
- [ ] Implementar mmap de dma-buf.
- [ ] Implementar begin/end CPU access.
- [ ] Tratar fechamento em ordens arbitrárias.
- [ ] Impedir acesso depois do detach.
- [ ] Testar compartilhamento entre dois processos e dois dispositivos.
- [ ] Testar import/export repetido sem leaks.

## 18. KMS e atomic modesetting

- [ ] Implementar enumeração completa de CRTCs, connectors, encoders e planes.
- [ ] Implementar protocolo de consulta em duas etapas.
- [ ] Copiar arrays userspace com validação completa.
- [ ] Implementar mode blobs e property blobs.
- [ ] Implementar lifecycle de framebuffers.
- [ ] Implementar properties e valores por objeto.
- [ ] Validar combinações de modos e formatos.
- [ ] Implementar atomic check sem alterar hardware.
- [ ] Fazer atomic commit transacional.
- [ ] Implementar rollback em falhas.
- [ ] Sincronizar vblank e page flip.
- [ ] Entregar eventos por descritor.
- [ ] Tratar unplug de monitor durante commit.
- [ ] Implementar EDID robusto e limitar parsing.
- [ ] Testar resoluções, refresh rates, múltiplos monitores e modos inválidos.

## 19. Futuro driver de GPU

- [ ] Definir uma camada de compatibilidade antes de importar um driver externo.
- [ ] Validar family, dispositivo e capabilities antes de inicializar blocos.
- [ ] Validar firmware, tamanho, versão e checksums.
- [ ] Implementar gerenciamento de memória de dispositivo sob pressão.
- [ ] Validar command streams, endereços, tamanhos, alinhamentos e domínios.
- [ ] Impedir command submission de acessar memória não autorizada.
- [ ] Implementar scheduler com dependências, prioridades, timeout e reset.
- [ ] Propagar device loss para userspace e recuperar jobs após reset.
- [ ] Integrar interrupções, fences, writeback, power e thermal management.
- [ ] Testar suspend/resume, hangs sintéticos e fuzzing de ioctls.
- [ ] Não anunciar compatibilidade userspace antes de testes reais.

---

# P2: subsistemas necessários para um sistema utilizável

## 20. Filesystems e armazenamento

- [ ] Garantir criação, truncamento, rename, unlink e diretórios com rollback.
- [x] Definir semântica de flush, sync e persistência.
  `dos_flush()` valida o handle e chama `ata_sync()`, que grava todas as páginas
  dirty do block cache e em seguida emite `FLUSH CACHE` ao dispositivo. Falhas de
  writeback ou da barreira são propagadas ao chamador como erro, sem declarar a
  persistência concluída.
- [ ] Tornar o cache de blocos seguro contra concorrência e mídia removida.
  Estado parcial: `bcache` protege lookup, substituição, escrita, flush e
  invalidação com um spinlock global. Entretanto, executa `raw_read()` e
  `raw_write()` enquanto mantém o spinlock com interrupções desabilitadas, o que
  é inseguro se o driver bloquear, depender de IRQ ou apresentar alta latência.
- [~] Implementar write-back e preservar blocos dirty após falha de flush.
  Evidência: vítimas dirty são escritas antes da substituição e
  `bcache_flush()` mantém o bit dirty quando o driver retorna erro. Faltam
  política periódica, ordenação, barreiras e integração confiável no shutdown.
- [x] Sincronizar consulta de estatísticas do cache.
  `bcache_dirty_count()` agora percorre as entradas sob `cache_lock`, impedindo
  que a contagem observe atualizações parciais durante escrita, substituição ou
  flush concorrente.
- [ ] Definir invalidação segura em erro e remoção de mídia.
  Estado: `bcache_invalidate_lba()` descarta inclusive uma entrada dirty sem
  reportar perda de dados. O contrato precisa separar descarte deliberado,
  erro de I/O, troca de mídia e invalidação após reset.
- [ ] Propagar erros reais de ATA, FAT32 e VFS sem convertê-los em sucesso.
- [ ] Validar BPB, FAT, clusters, LFN e tamanhos antes de confiar no disco.
- [ ] Rejeitar imagens FAT32 truncadas, cíclicas ou malformadas.
- [ ] Recuperar ou montar read-only após inconsistências detectadas.
- [ ] Testar desligamento durante escrita e corrupção deliberada de imagens.
- [ ] Documentar limites de nome, path, volume e tamanho de arquivo.

### Auditoria aprofundada de FAT32

- [x] Validar assinatura do setor de boot e tipo real do volume.
  O mount exige assinatura 0x55AA, campos estruturais exclusivos do BPB FAT32,
  versão suportada e pelo menos 65525 clusters de dados. Também rejeita volumes
  cuja FAT não comporte todas as entradas da data region ou use clusters além
  do intervalo representável sem conflitar com valores reservados.
- [x] Validar a tabela de partições antes de usar o primeiro LBA.
  O mount valida assinatura, status, tipo FAT32, LBA inicial, tamanho e overflow
  da primeira partição. Também rejeita BPBs cujo volume exceda a partição MBR e
  limita a região declarada ao espaço endereçável pelo driver ATA LBA28.
- [x] Não confundir VBR válido com MBR usando apenas bytes por setor.
  A detecção de superfloppy agora exige assinatura, instrução de salto e campos
  exclusivos do BPB FAT32. Caso contrário, o setor zero é tratado como MBR e sua
  entrada de partição precisa passar pela validação completa.
- [x] Validar `sectors_per_cluster`.
  O mount rejeita zero, valores acima de 128 e valores que não sejam potência
  de dois antes de usar o campo em multiplicações, loops ou offsets.
- [x] Validar `reserved_sectors`, `num_fats`, `fat_sz_32` e `root_clus` antes de
  marcar o filesystem pronto.
  Valores zero, root cluster reservado ou fora da data region e layouts cuja
  região FAT consome o volume agora abortam o mount antes de `fat32_ready`.
- [x] Calcular e armazenar os limites físicos do volume.
  O mount calcula e armazena o primeiro setor de dados e o limite exclusivo do
  volume. Conversões de cluster e acessos à FAT rejeitam setores que ultrapassem
  a região correspondente, o volume declarado ou o espaço LBA de 32 bits.
- [x] Detectar overflow em `cluster_to_sector()`.
  A conversão agora rejeita clusters menores que dois ou fora da data region,
  calcula o LBA em 64 bits e falha se o resultado não couber em `uint32_t`.
- [x] Validar cluster antes de ler ou escrever uma entrada FAT.
  `fat_read_entry()` e `fat_write_entry()` rejeitam números fora do intervalo
  derivado da data region antes de calcular setor e offset.
- [x] Derivar a quantidade de clusters dos setores de dados, não da capacidade
  teórica da FAT.
  `fat_alloc_cluster()` agora percorre somente os clusters da data region
  calculada no mount, sem usar entradas excedentes disponíveis na FAT.
- [x] Diferenciar cluster livre, bad cluster, reservado e EOC.
  `fat_next_cluster()` aceita EOC como término, mas rejeita clusters livres,
  reserved, bad e valores fora da data region antes de continuar uma chain.
- [x] Detectar ciclos em chains FAT.
  Liberação, obtenção do último cluster, leitura, escrita e buscas de diretório
  limitam cada travessia pela quantidade de clusters da data region. Uma chain
  cíclica ou sem EOC não pode mais manter esses caminhos em loop infinito.
- [x] Propagar erro em `fat_get_last_cluster()`.
  A função agora retorna status separado do cluster resultante e falha diante de
  erro de I/O, entrada inválida ou chain sem EOC dentro do limite do volume.
- [x] Implementar rollback em `fat_extend_chain()`.
  Se uma alocação ou atualização FAT falhar, a chain original volta a terminar
  no cluster que era seu último elemento e os clusters anexados são liberados.
- [x] Tornar atualização das múltiplas FATs consistente.
  `fat_write_entry()` preserva o valor anterior de cada mirror e tenta restaurar
  as cópias já alteradas quando uma escrita posterior falha. Se o rollback não
  puder ser concluído, o volume é marcado inconsistente e novas mutações falham.
- [x] Respeitar flags de mirroring e FAT ativa do BPB FAT32.
  O mount interpreta `BPB_ExtFlags`, valida o índice da FAT ativa e direciona
  leituras e escritas apenas à cópia ativa quando mirroring está desabilitado.
- [x] Preservar FSInfo ou declarar explicitamente que ele não é suportado.
  O mount valida assinaturas e localização do FSInfo. Antes da primeira mutação
  da FAT, free count e next-free são marcados como desconhecidos, evitando que
  ferramentas externas confiem em hints obsoletos enquanto o driver ainda não
  mantém contadores exatos.
- [x] Serializar alocação e mutações da FAT.
  Um mutex por volume cobre operações públicas de escrita, criação e remoção,
  mantendo procura e marcação de cluster livre na mesma seção crítica.
- [x] Serializar criação, remoção e expansão de diretórios.
  Criação, reserva/publicação de dirents, crescimento de diretórios e remoção
  executam sob o mesmo mutex, impedindo writers de reutilizarem os mesmos slots.
- [x] Corrigir buffer de inicialização de diretório para clusters maiores que um
  setor.
  `fat32_create_dirent()` agora inicializa o cluster setor por setor usando um
  buffer de exatamente 512 bytes, sem escrever além da stack quando
  `sectors_per_cluster` é maior que um.
- [x] Escrever conteúdo correto em cada setor ao inicializar diretório.
  As entradas `.` e `..` são escritas somente no primeiro setor. O buffer é
  zerado antes dos setores restantes, que não recebem cópias dessas entradas.
- [x] Liberar cluster de diretório quando sua inicialização falha.
  Se qualquer escrita de setor falhar, a chain recém-alocada é liberada antes
  de retornar erro e antes de publicar a entrada do diretório.
- [x] Implementar rollback ao crescer diretório.
  `fat32_dir_reserve()` agora inicializa e zera completamente o novo cluster
  antes de ligá-lo à chain. Falhas anteriores à publicação liberam o cluster.
- [x] Permitir sequências LFN atravessando fronteiras de setor e cluster.
  A reserva mantém uma posição física estável e acumula slots livres ao longo de
  setores e clusters consecutivos da chain. Se necessário, clusters zerados são
  anexados até completar o grupo; a escrita avança pela FAT entre cada slot.
  A suíte host-side cobre um grupo LFN iniciado nos dois últimos slots de um
  cluster e concluído no cluster recém-anexado.
- [x] Validar rigorosamente entradas LFN.
  O acumulador agora exige sequência estritamente decrescente e completa,
  LAST_LONG_ENTRY apenas na primeira entrada física, checksum uniforme e
  compatível com o short name, `type == 0`, cluster low igual a zero, terminador
  obrigatório, padding `0xFFFF`, limite de 255 caracteres e ausência de
  surrogate UTF-16 isolado. Grupos inválidos são ignorados em favor do nome 8.3.
- [x] Tratar UTF-16 corretamente.
  A interface VFS usa UTF-8. A criação valida UTF-8 canônico e converte code points
  suplementares em surrogate pairs UTF-16; a leitura valida os pares e reconstrói
  UTF-8 sem substituir caracteres por `?`. Sequências UTF-8 inválidas, surrogates
  isolados e nomes que excedem 255 unidades UTF-16 são rejeitados sem truncamento
  silencioso. A suíte host-side cobre round-trip de nome com caractere suplementar
  e rejeição de UTF-8 e pares inválidos. O cálculo de slots inclui explicitamente
  o terminador UTF-16, inclusive quando o nome ocupa exatamente 13 unidades por
  slot, evitando grupos LFN estruturalmente inválidos e fallback inesperado ao 8.3.
- [x] Validar nomes FAT32 na criação.
  Criação e remoção rejeitam componentes vazios, `.`/`..`, controles,
  caracteres proibidos pelo FAT, trailing spaces/dots e nomes que excedem o
  limite exposto pelo VFS. A suíte host-side cobre o subset aceito e rejeitado.
- [x] Tornar geração 8.3 livre de colisões sob concorrência.
  Criação de arquivo e diretório mantém geração do alias, teste de existência,
  reserva de slots e publicação do dirent sob `fat32_mutation_lock`. Assim, dois
  criadores não podem observar simultaneamente o mesmo alias como livre. A suíte
  host-side também verifica que nomes longos com o mesmo prefixo recebem aliases
  8.3 distintos e continuam acessíveis pelos nomes completos.
- [x] Diferenciar fim de diretório de erro de I/O.
  Funções de lookup retornam códigos equivalentes para nome ausente, fim de lista
  e falha de leitura, impedindo o VFS de propagar erro correto.
  O VFS agora oferece operações de diretório com status explícito: retorno positivo
  indica entrada encontrada, zero indica fim/ausência legítimos e retorno negativo
  transporta falhas. O FAT32 converte leitura de setor, chain inválida e corrupção
  em `-AOS_ERR_READ_ERROR`; `dos_ex_next()` preserva esse erro em vez de apresentá-lo
  como fim de diretório.
- [x] Diferenciar EOF de erro em leitura de arquivo.
  `fat32_file_read()` retorna zero tanto para condições legítimas quanto para
  falhas ao atravessar a FAT antes de copiar dados.
  EOF legítimo continua retornando zero, enquanto falhas de leitura de setor,
  conversão de cluster, corrupção ou término prematuro da chain retornam
  `-AOS_ERR_READ_ERROR` através do canal assinado já reconhecido pelo VFS.
- [x] Propagar erro de escrita mesmo após cópia parcial para o cache.
  O retorno atual em vários caminhos é apenas `bytes_written`, sem canal para
  indicar que a persistência falhou após parte da operação.
  Falhas de setor, travessia da FAT, publicação de dirent ou barreira de
  persistência retornam `-AOS_ERR_WRITE_ERROR`, mesmo se dados já tiverem sido
  copiados ao cache. O VFS não avança a posição para retornos negativos, evitando
  que uma escrita parcial com erro seja apresentada como sucesso silencioso.
- [x] Detectar overflow em `offset + size` e no novo tamanho do arquivo.
  A escrita rejeita requests cujo intervalo exceda `uint32_t` e também valida a
  soma usada para arredondar o tamanho à quantidade necessária de clusters.
- [x] Não atualizar `node->length` para o tamanho solicitado quando houve escrita
  parcial.
  O tamanho em memória e a atualização do dirent agora usam `offset +
  bytes_written`, sem publicar o fim originalmente solicitado após short write.
- [x] Remover o hack de atualização do dirent via raiz `/fat32`.
  A escrita não resolve mais a raiz nem procura o arquivo pelo nome. O dirent
  exato encontrado no lookup é atualizado, inclusive para arquivos em
  subdiretórios e quando existe um homônimo na raiz.
- [x] Armazenar localização estável do dirent no inode/open file.
  Nós FAT32 de arquivo guardam setor, índice e short name do dirent. A atualização
  ocorre sob o mutex de mutação e confirma que o slot ainda contém a mesma
  entrada, rejeitando referências obsoletas após unlink, rename ou reutilização.
- [x] Ordenar publicação de dados, FAT chain e tamanho do arquivo.
  Sem journal, deve existir uma estratégia explícita que minimize cross-links,
  lost chains e tamanho apontando para dados não persistidos após power loss.
  Crescimentos agora executam uma barreira de persistência depois de inicializar
  dados e atualizar a FAT, mas antes de publicar cluster inicial ou tamanho no
  dirent. Uma segunda barreira torna a publicação durável antes de atualizar o
  inode em memória e reconhecer bytes ao chamador. Falha antes da publicação
  conserva o dirent antigo; falha posterior deixa a durabilidade incerta, marca
  o volume inconsistente e não reconhece a escrita como concluída.
- [x] Implementar truncamento com ordem e rollback definidos.
  A redução precisa publicar primeiro tamanho seguro, depois liberar cauda; a
  expansão precisa inicializar novos dados para não expor conteúdo antigo.
  Escritas comuns não executam mais truncamento implícito quando sobrescrevem
  apenas um prefixo de um arquivo maior. A chain e o tamanho existente são
  preservados; redução ficará restrita a uma operação explícita de truncate com
  contrato próprio de publicação e rollback.
  `fat32_truncate()` implementa redução explícita com ordem metadata-first. O
  tamanho seguro e o cluster inicial são publicados antes de destacar e liberar
  a cauda. Falha na publicação preserva integralmente a chain; falha posterior de
  FAT nunca deixa o dirent apontando para clusters liberados e marca o volume
  inconsistente. A expansão aloca e zera o intervalo novo antes de cada publicação
  de tamanho; falhas intermediárias restauram o tamanho original por meio da
  redução segura, e falha nesse rollback marca o volume inconsistente.
- [x] Zerar clusters recém-alocados para arquivos.
  Todo cluster reservado pela FAT é zerado setor por setor antes de ser
  retornado ao chamador e publicado em uma chain. Se a inicialização falhar, a
  reserva é revertida; falha nesse rollback marca o volume inconsistente.
  A primeira escrita em arquivo vazio também contabiliza corretamente o cluster
  inicial, sem anexar um segundo cluster desnecessário. O inode em memória só é
  publicado após a atualização bem-sucedida do dirent; se essa publicação falhar,
  o cluster inicial ainda não publicado é liberado conservadoramente.
- [x] Validar diretório vazio antes de removê-lo.
  A remoção percorre toda a chain e aceita somente entradas apagadas, LFN e as
  entradas especiais `.` e `..`. Erros de leitura, corrupção ou qualquer filho
  vivo impedem a operação.
- [x] Impedir remoção ou rename de `.` e `..`.
  A validação comum de componentes rejeita ambos antes de entrar na seção
  crítica de mutação; a inspeção de diretório reconhece-os apenas como entradas
  especiais internas.
- [x] Atualizar `..` ao mover diretório entre pais.
  O rename FAT32 atualiza a entrada especial antes de retirar o nome antigo e
  restaura o pai anterior se a publicação final falhar. Movimentos para o próprio
  diretório ou para descendentes são rejeitados seguindo a cadeia de `..` com
  limite de travessia.
- [x] Implementar rename atômico ou documentar janela de inconsistência.
  O FAT32 publica primeiro o novo dirent e só depois aposenta o antigo. Assim,
  falha ou queda de energia pode deixar temporariamente dois nomes para a mesma
  chain, mas não perde o objeto nem libera seus dados. Falhas observadas em
  runtime tentam remover o destino e restaurar `..`; rollback incompleto marca o
  volume inconsistente. Arquivos abertos são rejeitados com DEVICE_BUSY enquanto
  a identidade de handles continuar baseada na posição física do dirent.
- [x] Garantir que unlink de arquivo aberto preserve lifetime ou seja rejeitado de
  forma definida.
  A política FAT32 definida é rejeitar unlink enquanto houver qualquer abertura da
  identidade estável do dirent. O registro é compartilhado entre nós produzidos por
  lookups distintos e protegido pelo mutex de mutação; a última operação de close
  libera a identidade e permite a remoção.
- [x] Impedir free de chain enquanto outro reader/writer ainda a percorre.
  Como unlink de arquivo aberto retorna `-AOS_ERR_DEVICE_BUSY`, a chain permanece
  alocada durante toda a lifetime dos handles usados por readers e writers.
- [ ] Definir comportamento quando apenas uma cópia da FAT pode ser lida.
- [ ] Montar read-only ao detectar divergência ou inconsistência não reparável.
- [x] Integrar `ata_sync()` e `bcache_flush()` à semântica real de fsync/sync.
  A syscall de flush passa pela camada DOS e somente retorna sucesso depois de
  `ata_sync()` concluir o writeback do `bcache` e a barreira `FLUSH CACHE` ATA.
- [x] Não executar flush de armazenamento diretamente em contexto de timer IRQ.
  O callback do PIT não chama mais `bcache_flush()`. Em cada intervalo ele apenas
  solicita um item de trabalho, mantendo I/O síncrono e a espera pelo dispositivo
  fora do contexto de hard IRQ.
- [x] Agendar writeback periódico em workqueue ou worker dedicado.
  O timer agenda um `work_struct` estático em `system_long_wq`, e o kworker chama
  `bcache_flush()` em contexto de tarefa. O contrato de `queue_work()` coalesce
  pedidos enquanto o item está pending ou running e impede duas execuções
  simultâneas do mesmo callback.
- [x] Preservar dirty state até confirmação completa do dispositivo.
  `bcache_flush()` e `bcache_flush_lba()` somente limpam o bit dirty depois que
  `raw_write()` confirma sucesso. Falhas preservam a entrada válida e dirty para
  nova tentativa; a substituição de vítima também é abortada se o writeback
  anterior falhar, evitando sobrescrever dados ainda não persistidos.
- [x] Definir barreira de cache do dispositivo antes de declarar sync concluído.
  `ata_sync()` emite o comando ATA `FLUSH CACHE` após o block cache ficar limpo e
  aguarda o dispositivo sair de busy; erro ou timeout impedem retorno de sucesso.
- [ ] Testar imagens FAT32 com sectors-per-cluster 1, 2, 4, 8, 16, 32, 64 e 128.
- [ ] Criar corpus de imagens malformadas para BPB, FAT, LFN e diretórios.
- [ ] Testar falha injetada em cada leitura e escrita de setor durante create,
  extend, truncate, rename, unlink e mkdir.
- [ ] Comparar volumes modificados pelo AgnusOS com `fsck.fat` após cada cenário
  de teste e após cortes de energia simulados.

### Auditoria aprofundada de workqueues e timers

- [~] Adicionar estado formal ao `work_struct`.
  Estado: `pending/running/cancelled/delayed/wq` já existem (`kernel/kernel/workqueue.c:95-99`),
  mas faltam `idle` formal, registry, flush-sync e regras de transição documentadas.
- [x] Rejeitar a inserção do mesmo work duas vezes.
  Evidência: `queue_work()` em `kernel/kernel/workqueue.c:87-93` rejeita se `pending||running`.
  Falta teste de estresse (enqueue duplicado, self-requeue).
- [ ] Preservar a workqueue de destino em delayed work.
  `queue_delayed_work(wq, ...)` ignora `wq` depois de validar o ponteiro; quando
  expira, `timerwheel_process()` sempre envia o trabalho para `system_wq`.
- [x] Fazer `cancel_work_sync()` remover trabalho pendente e aguardar execução.
  Evidência: `kernel/kernel/workqueue.c:122-156` remove da fila + espera `running`, sem zerar `func`.
  Falta definir retorno (pending removido / running drenado / inativo) e `cancel_delayed` no bucket.
- [ ] Fazer `cancel_delayed_work_sync()` remover o item do bucket correto.
- [ ] Definir retorno de cancelamento como pending removido, running drenado ou
  item já inativo.
- [ ] Não usar `func == NULL` como estado de cancelamento.
  Estado atual não zera mais `func` no cancel; manter invariante e cobrir com teste de reuso após cancel.
- [ ] Fazer `flush_workqueue()` aguardar também callbacks já em execução.
  O código atual apenas esvazia a lista e executa callbacks no próprio caller;
  não há contador de running nem barreira para outro drainer.
- [ ] Impedir dois callers de executar `flush_workqueue()` simultaneamente.
  Atualmente cada um pode retirar elementos e executar callbacks em paralelo,
  violando expectativa de workqueue ordenada.
- [ ] Garantir que `destroy_workqueue()` drene ou cancele sincronamente todo o
  trabalho.
  O implementation atual desconecta a lista e libera a queue, sem informar aos
  works e sem esperar callbacks. Delayed works destinados à fila também não são
  encontrados.
- [ ] Manter registry das workqueues alocadas ou criar worker por queue.
  O comentário afirma que o kworker drena filas de drivers, mas o loop executa
  apenas `system_wq` e `system_long_wq`. Workqueues retornadas por
  `alloc_workqueue()` não são drenadas automaticamente.
- [ ] Limitar e validar tamanho do nome da workqueue.
  `simple_strcpy()` copia até NUL em um array de 32 bytes sem limite, permitindo
  overflow de heap para nomes longos.
- [ ] Tratar rollback parcial em `workqueue_init()`.
  Se uma de duas alocações falha, a outra não é liberada e ponteiros globais
  podem ficar em estado parcialmente inicializado.
- [ ] Proibir queue em workqueue destruída ou em processo de shutdown.
- [ ] Implementar wakeup explícito do kworker.
  O trabalhador usa `hlt` e depende de qualquer IRQ futura, gerando latência não
  limitada e comportamento frágil se interrupções estiverem mascaradas.
- [ ] Não manipular `need_resched` global diretamente no kworker.
  Limpar o flag ali pode consumir pedido de reschedule destinado a outro caminho;
  a política deve pertencer ao scheduler.
- [x] Corrigir lifetime do timer usado por `timerwheel_register_timer()`.
  Evidência: timer periódico agora usa armazenamento estático (`kernel/kernel/workqueue.c:24,257-268`),
  sem `timer_entry_t` na stack. Itens abaixo seguem abertos para o caso geral.
- [x] Usar timer persistente ou alocado com lifetime explícito para o timerwheel.
- [ ] Não rearmar timer periódico criando objetos temporários a cada callback.
- [ ] Tornar `timer_remove()` sincronizado com callback em execução.
  Remover da lista não garante que o timer já retirado pelo IRQ não esteja prestes
  a chamar a função.
- [ ] Definir ownership de `timer_entry_t` após expiração.
  O timer core remove o objeto antes de chamar o callback, mas não sinaliza estado
  idle nem impede reuso concorrente.
- [ ] Tratar wraparound de ticks em comparações de expiração.
- [ ] Tratar delays maiores que o tamanho da timer wheel.
  O bucket é escolhido por módulo 512 e itens prematuros são reenfileirados; isso
  precisa ser testado para múltiplas voltas, wraparound e cancelamento.
- [ ] Evitar corrida entre o processamento de um bucket e novo enqueue para o
  mesmo tick.
- [ ] Garantir que `system_wq` exista antes de mover delayed work expirado.
- [ ] Definir contextos permitidos para queue, cancel, flush e destroy.
- [ ] Testar enqueue duplicado, self-requeue, cancel durante execução, destroy com
  delayed work e flush concorrente.
- [ ] Adicionar teste de estresse com milhões de transições e verificação de que
  cada work execute no máximo uma vez por enqueue aceito.

### Auditoria aprofundada de PCI, BARs e lifecycle de dispositivos

- [ ] Serializar acessos ao mecanismo PCI Configuration Mechanism 1.
  As portas 0xCF8 e 0xCFC formam uma operação composta global. Sem lock, dois
  acessos concorrentes podem escrever endereços diferentes em 0xCF8 e ler ou
  alterar o dispositivo errado.
- [ ] Tornar read-modify-write de config space atômico sob o mesmo lock.
  `pci_write_config_word()` lê um dword, altera metade e escreve o dword inteiro;
  outro writer concorrente pode perder atualização.
- [ ] Enumerar funções de acordo com multi-function bit.
  `pci_enum()` consulta apenas função zero e ignora funções 1 a 7, bridges e a
  topologia real de buses.
- [ ] Enumerar buses por bridges ou documentar explicitamente o scan bruto.
  O scan de todos os 256 buses pode encontrar dispositivos, mas não constrói
  ownership, recursos, hierarchy ou lifecycle de bridge.
- [ ] Criar `pci_device` persistente para cada função descoberta.
  Drivers precisam de objeto com BDF, IDs, class, revision, BARs, IRQ, estado,
  referências e callbacks de probe/remove.
- [ ] Separar enumeração de bind de driver.
  O core deve descobrir e validar recursos antes de permitir bus mastering ou
  registrar IRQs.
- [ ] Implementar rollback padronizado de probe.
  Falhas após mapear BAR, habilitar MSI, registrar IRQ ou alocar DMA devem desfazer
  recursos na ordem inversa.
- [ ] Implementar remove e shutdown callbacks idempotentes.
- [ ] Desabilitar bus mastering antes de liberar buffers DMA.
- [ ] Mascarar interrupções e sincronizar handlers antes de desmapear BARs.
- [ ] Detectar BAR de I/O versus memória sem aplicar máscara errada.
  `pci_get_bar_size()` usa sempre máscara `~0xF`, adequada a memory BAR, mas I/O
  BAR exige máscara distinta.
- [ ] Suportar e validar BARs de memória de 64 bits.
  O código lê apenas o dword baixo, descartando endereço e máscara altos. Isso
  pode mapear MMIO físico incorreto ou truncado acima de 4 GiB.
- [ ] Não consultar tamanho de BAR enquanto decode e bus master permanecem
  ativos sem coordenação.
  Escrever 0xFFFFFFFF em BAR ativo pode alterar temporariamente o endereço visto
  pelo dispositivo. O procedimento deve salvar command, desabilitar decode,
  sondar e restaurar todo o estado.
- [ ] Restaurar ambos os dwords ao sondar BAR de 64 bits.
- [ ] Validar índice de BAR contra o tipo de header.
  Functions comuns, bridges e CardBus possuem quantidade e significado distintos
  de BARs.
- [ ] Rejeitar BAR ausente ou endereço reservado.
- [ ] Não assumir tamanho de 4 KiB quando a sondagem falha.
  `pci_map_bar_wc()` substitui tamanho zero por 4096, podendo criar mapping
  inventado para recurso inexistente.
- [ ] Propagar falha de `vmm_map_region()` ao caller.
  O retorno do mapeamento é ignorado e `pci_map_bar_wc()` declara sucesso mesmo
  se nenhuma página tiver sido mapeada.
- [ ] Validar alinhamento de endereço virtual e físico antes do mapping.
- [ ] Tratar offset não alinhado do BAR sem perder bytes iniciais.
- [ ] Verificar overflow ao alinhar e converter tamanho para `uint32_t`.
  `pci_map_bar_wc()` calcula tamanho em 64 bits, mas passa `(uint32_t)size` para o
  VMM, truncando BARs maiores que 4 GiB.
- [ ] Manter objeto de mapping para validar `pci_unmap_bar()`.
  A API atual recebe endereço e tamanho arbitrários, sem confirmar ownership,
  cacheability ou correspondência com BAR previamente mapeado.
- [ ] Tornar o endereço virtual temporário de MSI-X exclusivo.
  Mask, unmask e enable usam o endereço fixo 0xFFFF800200000000 sem lock global;
  operações concorrentes podem remapear a janela uma sobre a outra.
- [ ] Mapear todas as páginas necessárias da tabela MSI-X.
  Mask/unmask mapeiam apenas 4096 bytes, mas indexam `vector * 16` sem validar se
  a entrada está dentro dessa página.
- [ ] Interpretar `vector` de mask/unmask como índice da tabela e validar contra
  table size.
- [ ] Validar que tabela MSI-X e PBA cabem integralmente no BAR indicado.
- [ ] Preservar offset intra-página ao mapear a tabela MSI-X.
  Mapear diretamente `table_base + table_offset` em virtual page-aligned exige
  ajustar o offset físico e o ponteiro resultante.
- [ ] Verificar falha do mapping da tabela antes de acessar MMIO.
- [ ] Inserir memory barriers e reads de confirmação quando exigidos pelo PCI/MSI-X.
- [ ] Programar entradas MSI-X mascaradas e só desmascarar após handler pronto.
  O código escreve vector control zero durante configuração, expondo interrupção
  antes que todo teardown/registro esteja necessariamente consistente.
- [ ] Desabilitar MSI ao habilitar MSI-X e vice-versa.
- [ ] Restaurar corretamente INTx somente se estava habilitado antes da mudança.
  `pci_disable_msi()` e `pci_disable_msix()` simplesmente limpam INTx disable,
  podendo alterar estado preexistente do dispositivo.
- [ ] Validar `num_vectors > 0`, ponteiro `vectors` e cada vector permitido.
- [ ] Corrigir codificação MSI-X APIC documentada e validá-la contra modo edge,
  trigger, delivery mode e destination APIC utilizados pelo kernel.
- [ ] Não aceitar silenciosamente um valor `vectors[i] >= 256` como message
  address alternativo sem uma estrutura de API explícita.
- [ ] Implementar alocação e liberação conjunta de vetores, handlers e capability.
- [ ] Integrar domínio IOMMU real ao objeto do dispositivo.
  `pci_iommu_map()` descarta BDF e passa domínio NULL, portanto não oferece
  isolamento por dispositivo.
- [ ] Restringir DMA ao address width anunciado e suportado pelo driver.
- [ ] Testar BAR 32-bit, BAR 64-bit, I/O BAR, bridge, capability cíclica, MSI e
  MSI-X com falha injetada em cada etapa.
- [ ] Testar teardown concorrente com IRQ pendente e atividade DMA.

## 21. Rede e sockets

- [~] Definir ownership e teardown de sockets por tarefa.
  Evidência: tarefas possuem `SocketBase` e `bsdsocket_task_exit()` percorre e
  fecha sockets. Falta confirmar que `task_exit()` chama esse teardown em todos
  os caminhos e proteger operações concorrentes.
- [ ] Validar todos os buffers e endereços fornecidos por userspace.
- [ ] Implementar checksums e validação rigorosa de tamanhos de pacotes.
  Estado parcial: IPv4 e ICMP validam checksum e comprimentos básicos. UDP gera
  checksum na transmissão, mas `udp_input()` não valida `uh->len`, checksum,
  comprimento mínimo ou se o datagrama cabe integralmente no pacote recebido.
- [ ] Validar IHL IPv4 antes de acessar opções e payload.
  Risco: `ip_input()` confirma versão e `pkt->len >= ihl`, mas não rejeita IHL
  menor que 20 bytes. Também é preciso validar `tot_len >= ihl` antes de calcular
  `tot_len - ihl`, evitando underflow.
- [ ] Rejeitar fragmentos IPv4 enquanto remontagem não existir.
  Estado: não há mecanismo de remontagem nem rejeição explícita baseada em MF e
  fragment offset. Protocolos superiores podem receber fragmentos como se fossem
  datagramas completos.
- [ ] Corrigir validação de ICMP antes de construir echo reply.
  O caminho de entrada valida o tamanho mínimo e checksum, mas o cálculo da
  resposta depende de campos IPv4 previamente aceitos. Comprimentos inválidos
  devem ser rejeitados antes de qualquer subtração ou alocação.
- [ ] Limitar filas, datagramas, sockets e memória por processo.
  Estado parcial: a implementação limita sockets UDP globais a 32 e descritores
  BSD por `SocketBase`. Entretanto, a fila RX de cada socket é ilimitada e usa
  alocações do heap do kernel, permitindo exaustão por flood.
- [ ] Tornar a fila RX FIFO e documentar a política de descarte.
  Risco: pacotes são inseridos no início da lista e removidos também do início,
  produzindo ordem LIFO. Faltam tamanho máximo, contadores de drop e política
  para descartar pacote novo ou antigo quando a fila estiver cheia.
- [ ] Corrigir locking aninhado de UDP.
  Risco confirmado: `udp_input()` reutiliza a mesma variável `flags` ao adquirir
  `udp_lock` e depois `sock->lock`. O segundo `spin_lock_irqsave()` sobrescreve o
  estado usado na restauração do primeiro lock, podendo restaurar incorretamente
  o estado de interrupções.
- [ ] Evitar corrida entre `udp_recvfrom()` e `udp_close()`.
  Um receiver pode dormir em `sock->wait` enquanto close limpa a fila e marca o
  slot como livre. Falta estado de fechamento, wakeup obrigatório e referência
  que impeça reutilização do slot por outro socket.
- [ ] Tratar fragmentação IPv4 ou rejeitá-la explicitamente.
- [ ] Implementar timeouts, cancelamento e wakeups sem perdas.
  Estado: `udp_recvfrom()` espera sem timeout e `bsdsocket` aceita interfaces de
  timeout/nonblocking que ainda não possuem comportamento completo. Fechamento,
  sinais e cancelamento precisam acordar waiters com erro definido.
- [ ] Resistir a pacotes truncados, sobrepostos e campos inconsistentes.
- [~] Validar frames Ethernet e ARP básicos.
  Evidência: `eth_input()` rejeita frames menores que o header Ethernet e
  `arp_input()` valida tamanho, hardware type, protocol type e tamanhos de
  endereço. Faltam validação de opcode, política contra spoofing e expiração
  exercitada periodicamente.
- [ ] Sincronizar registry de interfaces de rede.
  Risco: `netif_register()` protege a lista usando o lock da própria interface,
  mas buscas percorrem `netif_list` sem um lock global. Inserção, remoção futura
  e lookup concorrentes não têm contrato seguro de lifetime.
- [ ] Testar tráfego UDP prolongado, flood controlado e fechamento concorrente.
- [ ] Documentar claramente o subset BSD suportado.

### Driver RTL8139

- [ ] Adicionar timeout ao reset do hardware.
  Risco confirmado: `rtl8139_reset()` espera indefinidamente o bit RESET limpar.
  Hardware ausente ou defeituoso pode travar o boot inteiro.
- [ ] Validar todas as alocações DMA antes de programar o dispositivo.
  O init não verifica falha de `pmm_alloc_block()` para RX ou TX e converte o
  endereço físico diretamente para virtual. Endereço físico zero pode resultar
  em DMA para região inválida e corrupção.
- [ ] Alocar o ring RX com o tamanho realmente exigido pelo RTL8139.
  O código reserva um bloco físico de 4 KiB, mas configura e percorre um buffer
  RX de 8 KiB mais padding. Isso permite DMA além da página alocada.
- [ ] Aplicar restrição DMA32 e validar truncamento de endereço.
  Registradores RX/TX recebem endereços de 32 bits. As alocações precisam estar
  abaixo de 4 GiB e ser validadas antes do cast.
- [ ] Validar status e comprimento de cada frame RX.
  Comprimentos devem ser comparados com header mínimo, MTU, ring disponível e
  limites físicos antes de alocar ou copiar dados.
- [ ] Implementar ownership dos descritores TX.
  `rtl8139_send()` avança circularmente por quatro buffers sem verificar se o
  hardware concluiu a transmissão anterior, podendo sobrescrever um buffer em
  uso.
- [ ] Garantir que `net_xmit()` e o driver definam quem libera cada pacote.
  O contrato atual não deixa explícito se sucesso, erro ou conclusão assíncrona
  libera `net_pkt`, criando risco de leak ou double-free.
- [ ] Implementar teardown completo do dispositivo.
  Deve mascarar IRQ, remover handler, parar RX/TX, liberar buffers DMA, remover a
  interface e impedir callbacks depois da remoção.

## 22. Segurança, identidades e permissões

- [ ] Definir identidade de tarefa, usuário, grupo e credenciais.
- [ ] Definir permissões de arquivos, dispositivos, processos e IPC.
- [ ] Implementar checks de autorização em uma camada central.
- [ ] Definir capacidades mínimas para operações privilegiadas.
- [ ] Impedir acesso userspace irrestrito a I/O ports, MMIO, DMA e firmware.
- [ ] Inicializar memória antes de expô-la a outro domínio de segurança.
- [ ] Adicionar entropia e gerador de números aleatórios apropriado.
- [ ] Definir política de vulnerabilidades e canal de divulgação responsável.
- [ ] Produzir threat model do kernel e dos serviços suportados.

## 23. ACPI, energia e descoberta de hardware

- [ ] Validar checksums, comprimentos e ponteiros de todas as tabelas ACPI.
- [ ] Limitar acesso a memória física indicado por firmware não confiável.
- [ ] Implementar MADT e roteamento de interrupções de forma validada.
- [ ] Definir desligamento e reinicialização confiáveis.
- [ ] Implementar timers modernos antes de depender apenas do PIT.
- [ ] Tratar hardware ausente ou parcialmente inicializado sem panic.
- [ ] Documentar matriz de hardware e firmware validada.

## 24. SMP e escalabilidade

- [ ] Não habilitar SMP antes da auditoria de estado global e locks.
- [ ] Implementar inicialização de APs e estruturas per-CPU.
- [ ] Implementar scheduler por CPU e política de balanceamento.
- [ ] Implementar TLB shootdown e chamadas interprocessador.
- [ ] Definir afinidade de tarefas e interrupções.
- [ ] Validar atomics e barreiras de memória em todos os subsistemas.
- [ ] Testar boot e estresse com 1, 2, 4 e 8 CPUs virtuais.

## 25. Input, áudio, USB e GUI

- [ ] Separar eventos de input da implementação PS/2 específica.
- [ ] Definir fila de eventos por consumidor e proteção contra overflow.
- [ ] Projetar USB em camadas antes de adicionar controladores e classes.
- [ ] Projetar áudio com buffers, DMA, sincronização e underrun controlado.
- [ ] Definir compositor ou servidor gráfico fora do kernel quando possível.
- [ ] Manter desenho, widgets e políticas de janela fora do núcleo privilegiado.
- [ ] Não iniciar um driver GPU real antes dos requisitos P0 e P1.

# P3: qualidade, automação e operação

## 26. Testes automatizados

- [~] Proteger números e layouts básicos da ABI com `make abi-check`.
  Estado: `tests/uapi_abi.c` trava subset (traps-chave, 3 aliases, flags, ioctls e 2 layouts DRM) — não as 66 traps. Expandir para cobertura total antes da tag uAPI 1.0.
- [~] Possuir selftests de boot para DRM, GEM, fences, scheduler, IPC e assigns.
  Estado: existem, mas ainda dependem de inspeção da saída e rodam no boot normal.
- [ ] Criar target `make test` independente do build de release.
- [ ] Criar modo de kernel dedicado a selftests.
- [ ] Emitir resultado serial padronizado, por exemplo `TESTS: PASS`.
- [ ] Encerrar automaticamente o QEMU com `isa-debug-exit`.
- [ ] Definir timeout para detectar boot travado.
- [ ] Testar múltiplas quantidades de RAM, CPUs e dispositivos.
- [ ] Criar testes host-side para parsers, filas, bitmaps e estruturas.
- [ ] Criar testes ring 3 negativos para todas as interfaces públicas.
- [ ] Executar testes de estresse de memória, tarefas, IPC, VFS e rede.
- [ ] Medir leaks e crescimento de recursos entre início e fim de cada teste.

### Lacunas imediatas da infraestrutura de testes

1. separar selftests do boot interativo normal;
2. criar um agregador que contabilize pass/fail em vez de depender apenas de logs;
3. emitir uma linha final determinística pela serial;
4. encerrar QEMU com código observável pelo host;
5. adicionar timeout e preservar o log serial como artefato;
6. executar primeiro ABI, uaccess negativo, lifecycle de tarefas e VFS;
7. manter cada bug corrigido como caso permanente de regressão.

## 27. Fuzzing e injeção de falhas

- [ ] Criar harness host-side para ELF, FAT32, ACPI e protocolos de rede.
- [ ] Fazer fuzzing do dispatcher de syscalls e ioctls em ring 3.
- [ ] Injetar falhas determinísticas em alocações e I/O.
- [ ] Simular timeouts, interrupções perdidas, mídia removida e OOM.
- [ ] Preservar corpus de regressão para cada bug corrigido.
- [ ] Exigir que crashes produzam diagnóstico reproduzível.

## 28. Build, warnings e configurações

- [ ] Fazer o build ativo terminar com zero warnings.
- [ ] Adicionar configuração `debug` com asserts, poisoning e logs adicionais.
- [ ] Adicionar configuração `release` sem selftests destrutivos no boot.
- [ ] Tratar ferramentas ausentes com diagnóstico claro antes de iniciar o build.
- [ ] Separar o link do kernel da geração da ISO em targets explícitos.
- [ ] Tornar o build reproduzível com toolchain e versões documentadas.
- [ ] Gerar mapa de símbolos e preservar artefatos de debug por release.
- [ ] Executar `git diff --check`, ABI, build limpo e testes no CI.

## 29. Observabilidade e diagnóstico

- [ ] Padronizar níveis e formato do log do kernel.
- [ ] Adicionar timestamps e identificação de CPU/tarefa.
- [ ] Manter ring buffer de log consultável após o boot.
- [ ] Produzir backtrace confiável em panic e faults.
- [ ] Expor estatísticas de memória, scheduler, IRQs, VFS, rede e DRM.
- [ ] Implementar watchdog de hard lockup e detector de stalls.
- [ ] Permitir capturar diagnóstico integral pela serial.

## 30. Instalação, atualização e recuperação

- [ ] Definir layout de disco suportado e processo de instalação.
- [ ] Validar boot BIOS e UEFI de forma automatizada.
- [ ] Definir atualização transacional ou mecanismo seguro de rollback.
- [ ] Preservar configuração e dados do usuário entre versões.
- [ ] Criar modo de recuperação independente do sistema principal.
- [ ] Documentar backup, reparo de filesystem e recuperação de boot.

## 31. Releases e CI

- [ ] Criar pipeline para build limpo, ABI, QEMU e análise estática.
- [ ] Fixar versões da toolchain e dependências.
- [ ] Gerar checksums e manifesto de cada release.
- [ ] Assinar releases quando a infraestrutura estiver definida.
- [ ] Manter changelog técnico e notas de incompatibilidade.
- [ ] Arquivar binário, ISO, símbolos, headers públicos e resultados dos testes.
- [ ] Definir política de versões e suporte.

## 32. Documentação e governança

- [ ] Documentar arquitetura, boot, memória, scheduling, VFS e drivers.
- [ ] Registrar invariantes, ownership e locking de cada subsistema.
- [ ] Criar guia de contribuição, estilo e revisão.
- [ ] Exigir testes e documentação para alterações funcionais.
- [ ] Usar ADRs para decisões arquiteturais duradouras.
- [ ] Classificar componentes como suportados, experimentais ou históricos.
- [ ] Definir mantenedores por área e processo de revisão.

# Milestones propostas

## M2, desenvolvimento controlado

Critérios mínimos:

- auditoria completa dos ponteiros de todas as syscalls;
- build ativo sem warnings;
- target automatizado de testes em QEMU;
- ownership explícito para arquivos, sockets, ports e memória;
- documentação de ABI de traps e convenção de chamada;
- regressões conhecidas registradas como issues.

## M3, alpha

Critérios mínimos:

- isolamento demonstrado entre duas aplicações ring 3;
- faults userspace encerram somente a tarefa causadora;
- teardown sem leaks de tarefas e recursos;
- VFS com arquivos por abertura e handles por processo;
- testes negativos e de estresse executados no CI;
- boot BIOS e UEFI automatizado.

## M4, beta

Critérios mínimos:

- workloads reais executados por períodos prolongados;
- armazenamento testado contra corrupção e falhas de energia simuladas;
- segurança e permissões básicas aplicadas;
- instalação, atualização e recuperação documentadas;
- matriz de hardware suportado publicada;
- nenhum bloqueador P0 aberto.

# Trabalho iniciado

O primeiro épico ativo é **fronteira userspace/kernel**. A implementação base
já fornece limites de endereços de usuário, validação por página,
`access_ok()`, `copy_from_user()`, `copy_to_user()` e
`strncpy_from_user()`. Esses itens foram marcados acima com suas evidências.

Isso ainda não conclui o épico. A próxima etapa obrigatória é auditar todas as
interfaces públicas, eliminar desreferências diretas, definir semântica de
cópia parcial e adicionar testes ring 3 negativos. Até essa evidência existir,
o AgnusOS deve continuar classificado como M1 e não deve executar código não
confiável como se houvesse isolamento de produção.