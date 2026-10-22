# nvgd

**nvgd** é um driver gráfico baseado no Mesa, mantido de forma independente e
focado em GPUs NVIDIA **Turing e mais novas** (GTX 16xx, RTX 20xx/30xx/40xx).
OpenGL e Vulkan rodam de forma **nativa**, sem camada de tradução: OpenGL 4.6
pelo driver Gallium `nvc0` com o compilador de shaders **NAK**, Vulkan 1.4 pelo
**NVK**. Nada passa por Zink nem ANGLE.

O lado do kernel é o [nvgd-dkms](https://github.com/Geanziin/nvgd-dkms): um
fork do nouveau com suporte nativo ao firmware GSP-RM 615. Os pacotes para
Arch/CachyOS ficam em [nvgd-repo](https://github.com/Geanziin/nvgd-repo).

> Projeto independente. Não é afiliado ao Mesa, ao nouveau nem à NVIDIA e não
> é enviado para o Mesa upstream. A branch `upstream` acompanha o Mesa oficial
> e serve de base para rebases.

## Desempenho: nvgd × driver proprietário (RTX 3050)

Mesma máquina, mesma sessão GNOME, `glmark2` 2023.01 com a resolução padrão
(800x600).

| Modo | nvgd | NVIDIA 615.78.08 | nvgd / NVIDIA |
|---|---:|---:|---:|
| `glmark2 --off-screen` (só o driver, sem apresentação) | **35 236** | 31 803 | **1.11x** |
| `glmark2-wayland` (janela, Wayland nativo) | **9 902** | 2 653 | **3.73x** |
| `glmark2` (janela, X11/GLX via Xwayland) | **4 984** | 4 016 | **1.24x** |

Ambiente:

- GPU: GeForce RTX 3050 6 GB (GA107), PCIe 3.0 x8
- CPU: Intel Xeon E5-2697 v3
- Sistema: CachyOS, GNOME (Wayland)
- nvgd: kernel 7.2.11 + nvgd-dkms, Mesa nvgd
- Proprietário: kernel 6.18 LTS + `nvidia-open` 615.78.08

### Por cena (`--off-screen`)

O nvgd vence em 22 das 33 cenas.

| Cena | nvgd | NVIDIA 615.78.08 | nvgd / NVIDIA |
|---|---:|---:|---:|
| `build` use-vbo=false | 4887 | 4834 | **1.01x** |
| `build` use-vbo=true | 67300 | 53369 | **1.26x** |
| `texture` texture-filter=nearest | 51382 | 56129 | 0.92x |
| `texture` texture-filter=linear | 50478 | 54459 | 0.93x |
| `texture` texture-filter=mipmap | 55082 | 56177 | 0.98x |
| `shading` shading=gouraud | 50654 | 41633 | **1.22x** |
| `shading` shading=blinn-phong-inf | 49386 | 40908 | **1.21x** |
| `shading` shading=phong | 45700 | 37598 | **1.22x** |
| `shading` shading=cel | 46051 | 37349 | **1.23x** |
| `bump` bump-render=high-poly | 25109 | 22499 | **1.12x** |
| `bump` bump-render=normals | 73438 | 60836 | **1.21x** |
| `bump` bump-render=height | 67400 | 59266 | **1.14x** |
| `effect2d` kernel=0,1,0;1,-4,1;0,1,0; | 32915 | 40909 | 0.80x |
| `effect2d` kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 20601 | 22464 | 0.92x |
| `pulsar` light=false:quads=5:texture=false | 45799 | 41909 | **1.09x** |
| `desktop` blur-radius=5:effect=blur:passes=1:separable=... | 7563 | 4542 | **1.67x** |
| `desktop` effect=shadow:windows=4 | 13201 | 7998 | **1.65x** |
| `buffer` columns=200:interleave=false:update-dispersio... | 792 | 946 | 0.84x |
| `buffer` columns=200:interleave=false:update-dispersio... | 726 | 918 | 0.79x |
| `buffer` columns=200:interleave=true:update-dispersion... | 539 | 1057 | 0.51x |
| `ideas` speed=duration | 9204 | 10784 | 0.85x |
| `jellyfish`  | 21271 | 19205 | **1.11x** |
| `terrain`  | 1148 | 1327 | 0.87x |
| `shadow`  | 24634 | 12181 | **2.02x** |
| `refract`  | 2846 | 2865 | 0.99x |
| `conditionals` fragment-steps=0:vertex-steps=0 | 50868 | 45516 | **1.12x** |
| `conditionals` fragment-steps=5:vertex-steps=0 | 49038 | 44381 | **1.10x** |
| `conditionals` fragment-steps=0:vertex-steps=5 | 50229 | 44806 | **1.12x** |
| `function` fragment-complexity=low:fragment-steps=5 | 49961 | 44971 | **1.11x** |
| `function` fragment-complexity=medium:fragment-steps=5 | 47445 | 43534 | **1.09x** |
| `loop` fragment-loop=false:fragment-steps=5:vertex-s... | 49247 | 45091 | **1.09x** |
| `loop` fragment-steps=5:fragment-uniform=false:verte... | 49221 | 44681 | **1.10x** |
| `loop` fragment-steps=5:fragment-uniform=true:vertex... | 48726 | 44417 | **1.10x** |

Onde o proprietário ainda ganha:

- Cenas muito leves (`texture`, `effect2d`): limitadas pela latência de ida e
  volta à GPU. O driver da NVIDIA submete trabalho direto do userspace pelo
  doorbell; o nvgd ainda passa por um ioctl do kernel a cada frame (~4,5 µs
  nesta CPU).
- `ideas`: custo de CPU por draw no Mesa.
- `terrain`/`refract`: limitadas pela GPU, onde pesa a qualidade do código
  gerado pelo compilador de shaders.

Para reproduzir:

```sh
glmark2 --off-screen
glmark2-wayland
glmark2
```

## O que o nvgd muda em relação ao Mesa

- **NAK no OpenGL**: o compilador Rust do NVK compila os shaders do driver
  `nvc0` em Turing+. O que o NAK ainda não cobre (imagens, SSBOs, varyings
  legados do perfil compatibility) volta para o nv50_ir.
- **Submissão adiada**: com um único contexto, o trabalho é enviado no
  flush/fence em vez de a cada draw. Ainda assim é enviado cedo quando a GPU
  fica ociosa, e na hora quando envolve buffers dma-buf compartilhados.
- **Fences**: espera da fence por polling da página coerente antes de dormir no
  kernel, e reaproveitamento dos BOs de fence.
- **Compressão de cor** em render targets privados, com os limites de
  *reduce-color* sem perda.
- **Buffers**: mapeamento direto de buffers de VRAM ociosos em maps só de
  escrita.
- **Validação de estado**: correção de referências duplicadas de textura/UBO
  que cresciam sem limite.
- **Correções de renderização** para Chromium/Electron (Discord, Brave), GLX e
  GPU Screen Recorder.

### Variáveis de ambiente

| Variável | Padrão | Efeito |
|---|---|---|
| `NVGD_USE_NAK` | `true` | usa o NAK nos shaders OpenGL |
| `NVGD_DEFER_KICK` | `true` | submissão adiada |
| `NVGD_KICK_IDLE_MIN` / `NVGD_KICK_BATCH` | `96` / `256` | submissão antecipada quando a GPU está ociosa / limite do lote |
| `NVGD_FENCE_SPIN_US` | `200` | tempo de polling antes da espera no kernel |
| `NVGD_COLOR_COMPRESSION` | `true` | compressão de cor em Turing+ |
| `NVGD_DIRECT_VRAM_MAP` | `true` | map direto de buffers de VRAM ociosos |
| `NVGD_DEBUG_NAK` | `false` | log de qual compilador cada shader usou |

## Compilando

Igual ao Mesa (veja `README.mesa.rst` e <https://docs.mesa3d.org/install.html>):

```sh
meson setup build -Dgallium-drivers=nouveau,zink,softpipe -Dvulkan-drivers=nouveau
ninja -C build
```

## Contribuindo

Pull requests são bem-vindos. Abra o PR contra a branch `main`; todo PR é
revisado e aceito ou recusado pelo mantenedor. Para mudanças de desempenho,
inclua os números do `glmark2` antes/depois e o resultado dos testes de
renderização. Bugs e ideias vão em *Issues*.

## Licença

Mesmas licenças do Mesa (majoritariamente MIT; veja `docs/license.rst`).
