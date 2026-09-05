# ApolloOS — Roadmap: AmigaOS-like Moderno

> **Filosofia:** NÃO clonar AmigaOS 1.x/3.x. Capturar o *espírito* — APIs expressivas, IPC leve, developer experience prazerosa — e aplicar sobre uma base moderna (MMU, preempt, GPU). O usuário final não deve saber que o kernel é "AmigaOS-like"; o *desenvolvedor de drivers e apps* deve sentir que é.

---

## Princípios Fundamentais

| Princípio | AmigaOS Original | ApolloOS Moderno |
|-----------|------------------|------------------|
| **Memória** | Sem proteção, shared address space | MMU com per-process PML4, mas com shared memory regions |
| **Tasks** | ~500 bytes, sem endereço próprio | Leves para kernel threads, completas para userspace |
| **Sinais** | 32-bit bitmask entre tasks | Bitmask + POSIX signals (híbrido) |
| **IO** | Assíncrono via MsgPort | Assíncrono via io_uring-like + MsgPort compat |
| **Drivers** | Library + Unit + MsgPort | DRM-style + Library/Unit overlay |
| **Gráficos** | Custom chips (Copper/Blitter) | GPU via DRM/KMS + blitter software para 2D |
| **Scripts** | AREXX built-in | AREXX-like com shell integrado |

---

## Tier 1 — Essencial (Muda a API do kernel)

### 1.1 ExecBase / SysBase

O ponto central do sistema. Todo Amiga real tinha `SysBase` como ponteiro global para o estado do Exec.

```c
// kernel/include/exec.h
typedef struct exec_base {
    uint32_t        ecb_Version;     // 0x414F5300 = "AOS\0"
    uint32_t        ecb_Revision;

    // Listas globais
    list_head_t     ecb_LibraryList;   // todas as libraries abertas
    list_head_t     ecb_TaskList;      // todas as tasks
    list_head_t     ecb_InterruptList; // handlers por level (1-6)
    list_head_t     ecb_MemList;      // region types (chip/fast/public)

    // State
    uint32_t        ecb_IdleCount;
    uint32_t        ecb_TaskCount;
    uint32_t        ecb_LibCount;

    // Config
    uint32_t        ecb_MemTotal;
    uint32_t        ecb_MemFree;
    uint8_t         ecb_CpuCount;

    // Sinais
    spinlock_irq_t  ecb_SigLock;
} exec_base_t;

extern exec_base_t *SysBase;
```

**Syscalls:** `AOS_SysBase()` retorna o ponteiro.

**Impacto:** Baixo. É um struct global + init no boot. Toda a infra existente (tasks, msgports, assigns) passa a ser indexável via SysBase.

---

### 1.2 Library System

Cada subsistema do kernel é uma "library" aberta/fechada com refcount.

```c
// kernel/include/exec/library.h
#define LIBRARY_VERSION    1
#define LIBRARY_REVISION   0

typedef struct library {
    node_t          lib_Node;       // nome + links na lista
    uint32_t        lib_OpenCnt;    // quantas vezes aberta
    uint32_t        lib_Version;
    uint32_t        lib_Revision;
    uint32_t        lib_Flags;
    const char     *lib_IdString;
    void           (*lib_Init)(void);
    void           (*lib_Expunge)(void);
} library_t;

// Syscalls
library_t *AOS_OpenLibrary(const char *name, uint32_t version);
void       AOS_CloseLibrary(library_t *lib);
library_t *AOS_FindLibrary(const char *name);
```

**Libraries nativas do kernel:**
| Nome | Responsabilidade |
|------|------------------|
| `exec.library` | Gerenciamento de tasks, memória, interrupts |
| `dos.library` | VFS, assigns, path parsing, shell |
| `intuition.library` | Window manager, screens, gadgets |
| `graphics.library` | Framebuffer, blitting, fontes |
| `utility.library` | Strings, listas, hash tables |
| `amdgpu.library` | Driver GPU AMD |
| `timer.library` | Timer device, delays, VBlank |
| `keymap.library` | Teclado, layouts |

**Impacto:** Médio. Requer criar o registry de libraries e wrapping dos subsistemas existentes.

---

### 1.3 Signal Bitmask (híbrido)

O AmigaOS usava `Signal(task, mask)` com 32 bits. Nós mantemos POSIX signals E adicionamos bitmask.

```c
// kernel/include/exec/signals.h
#define SIGBREAKF_CTRL_C   (1 << 15)  // Ctrl+C (igual AmigaOS)
#define SIGBREAKF_CTRL_D   (1 << 16)
#define SIGBREAKF_CTRL_Z   (1 << 17)
#define SIGFPE             (1 << 8)   // floating point exception
#define SIGUSR1            (1 << 29)  // user-defined
#define SIGUSR2            (1 << 30)  // user-defined
#define SIGTERM            (1 << 31)  // termination

// Syscalls
int  AOS_Signal(task_t *task, uint32_t mask);
uint32_t AOS_Wait(uint32_t mask);
uint32_t AOS_WaitMask(uint32_t mask, uint64_t timeout_ms);
```

**Comportamento:** `AOS_Wait(bitmask)` bloqueia a task até que QUALQUER bit do bitmask seja setado por outra task. Quando retorna, os bits setados permanecem (caller deve limpar com `AOS_SigClear`). Compatível com POSIX: `AOS_Signal(task, SIGUSR1)` também levanta o signal handler POSIX.

**Impacto:** Médio. Adiciona nova API sobre o scheduler existente. Não remove POSIX signals.

---

### 1.4 IORequest Assíncrono

O AmigaOS fazia IO todo assíncrono via msgports. Nós adaptamos para o mundo moderno.

```c
// kernel/include/exec/io.h
typedef struct io_request {
    node_t          io_Node;
    library_t      *io_Device;
    uint32_t        io_Command;
    int32_t         io_Error;
    uint32_t        io_Actual;     // bytes transferidos
    msgport_t      *io_MsgPort;   // porta de completion
    uint8_t         io_Data[];
} io_request_t;

// Comandos de device
#define CMD_READ       1
#define CMD_WRITE      2
#define CMD_SEEK       3
#define CMD_CLEAR      4
#define CMD_WRITEYNC   5

// Syscalls
int  AOS_DoIO(io_request_t *req);          // síncrono (bloqueia)
int  AOS_SendIO(io_request_t *req);        // assíncrono
int  AOS_WaitIO(io_request_t *req);        // espera completion
int  AOS_CheckIO(io_request_t *req);       // não-bloqueante
void AOS_AbortIO(io_request_t *req);       // cancela
```

**Modelo de device:**
```
Device
├── Unit 0  ── MsgPort ── IORequests
├── Unit 1  ── MsgPort ── IORequests
└── Unit 2  ── MsgPort ── IORequests
```

Exemplo: `disk.device` com 4 units (uma por partição), cada unit com sua msgport. O driver DMGabriel enfileira IORequests na msgport da unit e sinaliza completion via `AOS_ReplyMsg`.

**Compatibilidade:** Cada fd POSIX vira um IORequest interno. `read()/write()` são wrappers síncronos sobre `SendIO()+WaitIO()`.

**Impacto:** Grande. Requer refatorar VFS para async I/O.

---

## Tier 2 — Visível (Faz parecer AmigaOS)

### 2.1 Memory Pools

```c
// kernel/include/exec/pools.h
#define MEMF_CHIP     (1 << 0)   // DMA-accessible (GPU, devices)
#define MEMF_FAST     (1 << 1)   // CPU-only, mais rápido
#define MEMF_PUBLIC   (1 << 2)   // qualquer task pode acessar
#define MEMF_CLEAR    (1 << 3)   // zera na alocação
#define MEMF_REVERSE  (1 << 4)   // aloca do fim pra trás

typedef struct mem_pool pool_t;

pool_t *AOS_CreatePool(uint32_t flags, uint32_t pudgeSize, uint32_t threshSize);
void   *AOS_AllocPooled(pool_t *pool, uint32_t size);
void    AOS_FreePooled(pool_t *pool, void *mem, uint32_t size);
void    AOS_DeletePool(pool_t *pool);

// Convenience
void   *AOS_AllocMem(uint32_t size, uint32_t flags);
void    AOS_FreeMem(void *mem, uint32_t size);
```

**Mapeamento para hardware:**
| Flag | Polaris RX 590 GME | vGPU |
|------|-------------------|------|
| `MEMF_CHIP` | VRAM BAR1 (256MB) | VRAM carveout |
| `MEMF_FAST` | RAM do sistema | RAM do sistema |
| `MEMF_PUBLIC` | RAM (compartilhado) | RAM |

**Impacto:** Médio. Mapeia sobre PMM/GEM existentes.

---

### 2.2 Datatypes

```c
// kernel/include/datatypes/datatypes.h
typedef struct datatype {
    const char     *dt_Name;        // "Text", "IFF-ILBM", "MPEG"
    const char     *dt_GroupID;     // "TEXT", "ILBM", "MPEG"
    uint32_t        dt_Priority;    // ordem de tentativa
    const uint8_t  *dt_Magic;       // bytes mágicos
    uint32_t        dt_MagicLen;
    const char    **dt_Extensions;  // {".txt", ".text", NULL}
    int           (*dt_Load)(const char *path, void **obj);
    void          (*dt_Free)(void *obj);
    int           (*dt_Draw)(void *obj, int x, int y);
    const char    *(*dt_ScreenFormat)(void *obj);
} datatype_t;

// Registros
void dt_register(const datatype_t *dt);
void dt_unregister(const datatype_t *dt);

// API
void *dt_load(const char *path);
void  dt_free(void *obj);
int   dt_draw(void *obj, int x, int y);
const char *dt_screen_format(void *obj);
```

**Datatypes built-in:**
| Nome | Grupo | Magic | Extensões |
|------|-------|-------|-----------|
| ASCII | TEXT | nenhum (fallback) | `.txt`, `.text`, `.md` |
| IFF-ILBM | ILBM | `FORM` + `ILBM` | `.iff`, `.lbm`, `.ilbm` |
| GIF | GIF | `GIF87a`/`GIF89a` | `.gif` |
| PNG | PNG | `\x89PNG` | `.png` |
| BMP | BMP | `BM` | `.bmp`, `.ico` |
| ELF | ELF | `\x7fELF` | `.elf`, `.o` |
| MIDI | MIDI | `MThd` | `.mid`, `.midi` |
| WAD | WAD | `IWAD`/`PWAD` | `.wad` |

**Impacto:** Médio. Útil para `dogin Type` e para futuros apps gráficos.

---

### 2.3 Path Parsing (AmigaDOS-style)

```c
// kernel/include/dos/path.h
char *dos_PathPart(const char *path);    // "Work:docs/file.txt" → "file.txt"
char *dos_FilePart(const char *path);   // "Work:docs/file.txt" → "file.txt"
char *dos_AddPart(const char *dir, const char *file, uint32_t bufsize); // join
int   dos_NameFromLock(const char *lock, char *name, uint32_t len);
int   dos_ParsePatternNoCase(const char *pat, char *buf, uint32_t len);
int   dos_MatchPatternNo(const char *pat, const char *str);
```

**Wildcard AmigaOS:** `#?.txt` = qualquer coisa + `.txt`. Implementação via regex simplificado.

**Impacto:** Baixo. Funções puras sem side effects.

---

### 2.4 Dogin Melhorado

| Feature | Status | Prioridade |
|---------|--------|------------|
| `Run` funcional (ELF Ring3) | ❌ Stub | Alta |
| I/O redirection (`>`, `<`, `>>`) | ❌ | Alta |
| Pipes (`\|`) | ❌ | Alta |
| Path search (`C:`) | ❌ | Média |
| Variáveis de ambiente | ❌ | Média |
| `Which` (encontra comando) | ❌ | Média |
| `Echo` com `$var` expansion | ❌ | Baixa |
| `If`/`Then`/`Else`/`EndIf` | ❌ | Baixa |
| `While`/`EndWhile` loops | ❌ | Baixa |
| `Run` em background (`&`) | ❌ | Baixa |
| `Wait` (espera background) | ❌ | Baixa |

**Estrutura de uma shell moderna AmigaDOS-like:**
```amigaos
; Setup script
Assign Work: HD0:Projects/apollo
Assign Sys: HD0:apolloOS/kernel
C:MakeDir Work:docs Work:src

; Build
Path C:
Run build.sh > build.log
If $Status = 0
    Echo "Build OK!"
    Run C:test_app
Else
    Echo "Build failed"
    Type build.log
EndIf
```

**Impacto:** Médio. dogin.c já tem a estrutura, precisa de parsing de redirection/pipes.

---

### 2.5 Layers (damaged-rectangle rendering)

```c
// kernel/include/intuition/layers.h
typedef struct layer {
    int32_t    l_Name;
    int32_t    l_TopEdge, l_LeftEdge, l_Width, l_Height;
    uint8_t   *l_Bitmap;          // buffer off-screen
    uint8_t   *l_BackBitmap;      // double buffer
    region_t   l_DamageRegion;    // retângulos sujos
    uint32_t   l_Flags;
    layer_t   *l_Super;           // layer pai
    layer_t   *l_Lower;           // layer abaixo na stack
    window_t  *l_Window;
} layer_t;

layer_t *layer_create(int left, int top, int width, int height, uint32_t flags);
void     layer_delete(layer_t *layer);
void     layer_set_position(layer_t *layer, int left, int top);
void     layer_damage(layer_t *layer, int x, int y, int w, int h);
void     layer_flush(void);      // repinta só as damage regions
```

**Otimização:** Em vez de repintar a tela toda a cada frame, só repintar os retângulos sujos. Para 60fps com 5 janelas, é a diferença entre 30fps e 60fps.

**Impacto:** Médio. O window manager atual (`wm.c`) pode ser refactorado para usar layers.

---

### 2.6 Screens (AmigaOS-style)

```c
// kernel/include/intuition/screens.h
typedef struct screen {
    char        scr_Title[64];
    int32_t     scr_Width, scr_Height;
    uint8_t     scr_Depth;        // bits por pixel
    uint32_t    scr_Flags;
    palette_t  *scr_ColorMap;     // 256 cores
    layer_t    *scr_Layer;
    window_t   *scr_FirstWindow;
    screen_t   *scr_NextScreen;   // stack de screens
} screen_t;

screen_t *screen_open(int width, int height, uint8_t depth, const char *title);
void      screen_close(screen_t *scr);
void      screen_to_front(screen_t *scr);
void      screen_to_back(screen_t *scr);
void      screen_drag(screen_t *scr);  // arrasta screen pra cima/baixo
```

**Comportamento AmigaOS:** Screens empilham. Ao arrastar uma screen pra cima, ela revela a screen de trás (como slots).

**Impacto:** Médio. Requer refatorar o framebuffer driver.

---

## Tier 3 — Avançado (Só quando Tier 1-2 estiverem sólidos)

### 3.1 BOOPSI (Object System)

```c
// kernel/include/intuition/class.h
typedef struct class {
    node_t          cl_Node;
    const char     *cl_Name;
    uint32_t        cl_Superclass;  // herança
    uint32_t        cl_ObjectSize;
    int           (*cl_Method)(object_t *obj, uint32_t method, ...);
    uint32_t        cl_Flags;
} class_t;

// API
class_t *boopsi_make_class(const char *name, const char *super, uint32_t objsize);
object_t *boopsi_new_object(class_t *cl, const char *tags, ...);
uint32_t boopsi_get_attr(object_t *obj, uint32_t attr);
void     boopsi_set_attrs(object_t *obj, const char *tags, ...);
uint32_t boopsi_do_method(object_t *obj, uint32_t method, ...);
void     boopsi_dispose_object(object_t *obj);
```

**Classes built-in:**
| Classe | Super | Método principal |
|--------|-------|-----------------|
| `rootclass` | — | `OM_NEW`, `OM_DISPOSE`, `OM_GET`, `OM_SET` |
| `buttongadget` | `rootclass` | `GM_RENDER`, `GM_HITTEST` |
| `stringgadget` | `rootclass` | `GM_KEYINPUT`, `GM_RENDER` |
| `scrollergadget` | `rootclass` | `GM_LAYOUT`, `GM_RENDER` |
| `checkboxclass` | `rootclass` | `GM_RENDER` |
| `imageclass` | `rootclass` | `GM_RENDER` |

**Impacto:** Grande. Sistema OO completo.

---

### 3.2 AREXX-like Scripting

```c
// kernel/include/rexx/rexx.h
typedef struct rexx_port {
    msgport_t  *rx_Port;
    char        rx_Name[64];
    uint32_t    rx_Flags;
} rexx_port_t;

// API
rexx_port_t *rexx_create_port(const char *name);
void         rexx_delete_port(rexx_port_t *port);
int          rexx_send(rexx_port_t *host, const char *command, char *result, uint32_t reslen);
int          rexx_wait(rexx_port_t *port, char *cmd_buf, uint32_t buflen);
```

**Exemplo de integração com dogin:**
```amigaos
; No dogin:
RX "REQUESTSTRING 'Qual seu nome?' 'Mundo'"
; RX envia para rexx.port (host = dogin)
; Dogin processa e retorna resultado
; RX imprime: "Olá, Mundo!"
```

**Impacto:** Grande. Requer parser de linguagem.

---

### 3.3 Blitter (2D acceleration)

```c
// kernel/include/graphics/blit.h
#define MINTERM_COPY    0x00   // source
#define MINTERM_FILL    0xF0   // pattern fill
#define MINTERM_OR      0xEE   // OR
#define MINTERM_AND     0x88   // AND
#define MINTERM_XOR     0x55   // XOR
#define MINTERM_COOKIE  0xCA   // cookie (mask-based)

void gfx_blt(uint8_t *dst, int32_t dstx, int32_t dsty, int32_t dstmod,
              uint8_t *src, int32_t srcx, int32_t srcy, int32_t srcmod,
              int32_t width, int32_t height, uint8_t minterm);
void gfx_blt_mask(uint8_t *dst, int32_t dstx, int32_t dsty, int32_t dstmod,
                   uint8_t *src, int32_t srcx, int32_t srcy, int32_t srcmod,
                   uint8_t *mask, int32_t maskx, int32_t masky, int32_t maskmod,
                   int32_t width, int32_t height, uint8_t minterm);
void gfx_area_fill(uint8_t *dst, int32_t dstx, int32_t dsty, int32_t dstmod,
                    int32_t width, int32_t height, uint32_t color);
```

**Implementação:** Via `memcpy`/SIMD para 2D, ou GPU SDMA quando disponível.

**Impacto:** Médio. Essencial para performance de Intuition.

---

### 3.4 Audio (Paula-like)

```c
// kernel/include/audio/audio.h
#define AUDIO_CHANNELS  4   // como o Paula original
#define AUDIO_RATE_8000   8000
#define AUDIO_RATE_11025  11025
#define AUDIO_RATE_22050  22050
#define AUDIO_RATE_44100  44100

typedef struct audio_channel {
    uint8_t  *ac_Data;
    uint32_t  ac_Length;     // bytes
    uint32_t  ac_Rate;       // Hz
    uint8_t   ac_Volume;     // 0-64
    uint8_t   ac_Bits;       // 8 ou 16
    uint8_t   ac_Stereo;     // 0=mono, 1=stereo
    uint8_t   ac_Flags;
    msgport_t *ac_MsgPort;   // completion
} audio_channel_t;

// API
int  audio_open(uint32_t channel, uint32_t rate, uint8_t bits);
void audio_play(uint32_t channel, audio_channel_t *ach);
void audio_stop(uint32_t channel);
void audio_set_volume(uint32_t channel, uint8_t vol);
```

**Implementação:** Via PC speaker (beep simples) para debug, ou HDA/intel audio se disponível.

**Impacto:** Médio. Não essencial para boot, mas faz o sistema "vivo".

---

## Ordem de Implementação Sugerida

```
Mês 1-2:  SysBase + Library system + Signal bitmask
          → API do kernel fica "AmigaOS-like"

Mês 3-4:  Memory Pools + Datatypes + Path Parsing
          → dos.library funcional

Mês 5-6:  Dogin melhorado (pipes, redirect, env vars)
          → shell é usável para development

Mês 7-9:  IORequest async + Layers + Screens
          → Intuition aparece

Mês 10+:  BOOPSI + AREXX + Blitter + Audio
          → sistema completo
```

---

## O que NÃO fazer

| Anti-padrão | Por quê |
|-------------|---------|
| Shared address space (sem MMU) | Quebra GPU port, IOMMU, segurança |
| Remover preempt/POSIX signals | Quebra drm_sched, firmware loading |
| Copper display list | Só faz sentido com hardware custom (FPGA) |
| Chip/Fast RAM split real | Moderno tem VRAM vs RAM, não chip/fast |
| `Forbid()/Permit()` global | Quebra preempt, não escala em SMP |
| Auto-vectors 1-6 | x86 usa APIC, não level-based auto-vectors |

---

## Referências

- **AmigaOS Wiki:** https://wiki.amigaos.net/
- **Exec Developer Docs:** https://wiki.amigaos.net/wiki/AmigaOS_Manual:_Exec
- **Intuition Developer Docs:** https://wiki.amigaos.net/wiki/AmigaOS_Manual:_Intuition
- **AREXX Manual:** https://wiki.amigaos.net/wiki/ARexx_Complete_Reference
- **AmigaDOS Reference:** https://wiki.amigaos.net/wiki/AmigaOS_Manual:_AmigaDOS

---

*Documento vivo — atualizar a cada milestone.*
