# nvgd

**nvgd** is an independent, Mesa-based graphics driver focused on NVIDIA
**Turing and newer** GPUs (GTX 16xx, RTX 20xx/30xx/40xx). OpenGL and Vulkan
run **natively** with no translation layer: OpenGL 4.6 through the `nvc0`
Gallium driver with the **NAK** shader compiler, and Vulkan 1.4 through
**NVK**. Nothing goes through Zink or ANGLE.

The kernel side is [nvgd-dkms](https://github.com/Geanziin/nvgd-dkms), a
nouveau fork with native GSP-RM 615 firmware support. Arch/CachyOS packages
live in [nvgd-repo](https://github.com/Geanziin/nvgd-repo).

> This is an independent project. It is not affiliated with Mesa, nouveau or
> NVIDIA and is not submitted to upstream Mesa. The `upstream` branch tracks
> official Mesa and is used as the rebase base.

## Performance: nvgd vs. the proprietary driver (RTX 3050)

Same machine and GNOME/Wayland desktop. The OpenGL rows below are the existing
`glmark2` 2023.01 measurements at its default 800x600 resolution.

| Mode | nvgd | NVIDIA 615.78.08 | nvgd / NVIDIA |
|---|---:|---:|---:|
| `glmark2 --off-screen` (driver only, no presentation) | **35,236** | 31,803 | **1.11x** |
| `glmark2-wayland` (windowed, native Wayland) | **9,902** | 2,653 | **3.73x** |
| `glmark2` (windowed, X11/GLX via Xwayland) | **4,984** | 4,016 | **1.24x** |
| `vkmark` (Vulkan, native Wayland, 800x600, immediate) | **14,076** | 12,925 | **1.09x** |
| `vkmark` (Vulkan, NVK re-measurement, 2026-10-23) | **15,510** | not re-measured | — |

The Vulkan result is the **median of three complete vkmark 2025.01 runs**:
14,076 / 13,401 / 14,090, measured on 2026-10-22 (UTC-03). NVIDIA 615.78.08
scored 13,399 / 12,925 / 12,792 (median **12,925**) after reboot, with identical
benchmark binary/data hashes and settings. NVK's aggregate score is **8.9% higher
in this workload**, not in every scene: NVIDIA leads in 8 of the 13 scene medians.
The kernel differs (nvgd: 7.3; NVIDIA: 6.18 LTS), and these are normal desktop
sessions, not an isolated driver-only comparison. Scores from vkmark and glmark2
are **not comparable across benchmarks**, and neither score measures input latency.
[Vulkan settings, raw logs and the matching NVIDIA procedure](docs/benchmarks/vulkan-2026-10-22/README.md).

A fresh NVK-side re-measurement on 2026-10-23, with the same vkmark binary,
plugin and data hashes, scored 15,510 / 15,549 / 15,497 (median **15,510**).
This is **not** a new paired result: NVIDIA was not re-run under these
conditions, so the 1.09x comparison above remains the reference. The higher
NVK score reflects the current system state, not a driver-code change.
[Re-measurement logs and environment](docs/benchmarks/vulkan-2026-10-23/README.md).

Setup:

- GPU: GeForce RTX 3050 6 GB (GA107), PCIe 3.0 x8
- CPU: Intel Xeon E5-2697 v3
- OS: CachyOS, GNOME (Wayland)
- nvgd OpenGL rows: kernel 7.2.11 + nvgd-dkms, nvgd Mesa
- nvgd Vulkan row: kernel 7.3.0-3-cachyos, nvgd-dkms 1.0.0-20,
  Mesa `a1c7b37fcb7b`, vkmark `1:2025.01-2.1`; fixed `B8G8R8A8Unorm`
  format, 13 default scenes at 10 seconds each, no overlay/validation layers
- Proprietary: kernel 6.18 LTS + `nvidia-open` 615.78.08

### Per scene (`--off-screen`)

nvgd wins 22 of the 33 scenes.

| Scene | nvgd | NVIDIA 615.78.08 | nvgd / NVIDIA |
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

Where the proprietary driver still leads:

- Very light scenes (`texture`, `effect2d`) are bound by the round trip to
  the GPU. NVIDIA's driver submits work straight from userspace through the
  doorbell, while nvgd still goes through a kernel ioctl every frame
  (~4.5 µs on this CPU).
- `ideas` is bound by per-draw CPU cost in Mesa.
- `terrain` and `refract` are GPU-bound, so shader code quality from the
  compiler matters most.

To reproduce:

```sh
glmark2 --off-screen
glmark2-wayland
glmark2
```

Vulkan (three full runs, with a three-second cube warm-up before each):

```sh
python3 docs/benchmarks/run-vkmark.py nvgd /path/to/new-nvgd-results
# After rebooting into NVIDIA, from a Wayland desktop:
python3 docs/benchmarks/run-vkmark.py nvidia /path/to/new-nvidia-results \
  --reference docs/benchmarks/vulkan-2026-10-22/nvgd/environment.json
```

## What nvgd changes compared to Mesa

- **NAK for OpenGL**: NVK's Rust compiler compiles the `nvc0` driver's shaders
  on Turing+. Anything NAK does not cover yet (images, SSBOs, legacy
  compatibility-profile varyings) falls back to nv50_ir.
- **Deferred submission**: with a single context, work is submitted at
  flush/fence time instead of after every draw. It is still submitted early
  once the GPU goes idle, and immediately when it touches shared dma-buf
  buffers.
- **Fences**: fence waits poll the coherent fence page before sleeping in the
  kernel, and fence BOs are recycled.
- **Colour compression** on private render targets, with lossless
  *reduce-color* thresholds.
- **Buffers**: idle VRAM buffers are mapped directly for write-only maps.
- **State validation**: fixed duplicated texture/UBO references that grew
  without bound.
- **Rendering fixes** for Chromium/Electron (Discord, Brave), GLX and
  GPU Screen Recorder.
- **Native video**: H.264 and AV1 VA-API decode submit directly to NVDEC through
  Gallium/DRM, without loading NVK or translating to Vulkan Video. H.264
  encode uses NVENC directly, including separate Gallium input planes.
  [Architecture, validated limits and codec roadmap](docs/native-video.md).

### Environment variables

| Variable | Default | Effect |
|---|---|---|
| `NVGD_USE_NAK` | `true` | use NAK for OpenGL shaders |
| `NVGD_DEFER_KICK` | `true` | deferred submission |
| `NVGD_KICK_IDLE_MIN` / `NVGD_KICK_BATCH` | `96` / `256` | early submission when the GPU is idle / batch cap |
| `NVGD_FENCE_SPIN_US` | `200` | polling time before the kernel fence wait |
| `NVGD_COLOR_COMPRESSION` | `true` | colour compression on Turing+ |
| `NVGD_DIRECT_VRAM_MAP` | `true` | direct maps of idle VRAM buffers |
| `NVGD_DEBUG_NAK` | `false` | log which compiler each shader used |

## Building

Same as Mesa (see `README.mesa.rst` and <https://docs.mesa3d.org/install.html>):

```sh
meson setup build -Dgallium-drivers=nouveau,zink,softpipe -Dvulkan-drivers=nouveau
ninja -C build
```

## Contributing

Pull requests are welcome. Open them against `main`; every PR is reviewed and
accepted or declined by the maintainer. For performance changes, include
`glmark2` numbers before and after plus the results of the rendering tests.
Use *Issues* for bugs and ideas.

## License

Same licenses as Mesa (mostly MIT; see `docs/license.rst`).
