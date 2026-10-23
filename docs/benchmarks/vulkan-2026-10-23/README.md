# NVK Vulkan re-measurement: RTX 3050 / GA107

This directory records a fresh **NVK-side** run of the same vkmark workload used
in the paired [2026-10-22 comparison](../vulkan-2026-10-22/README.md). It is not
a new paired NVIDIA comparison: the NVIDIA side was not re-run under these
conditions. The paired 2026-10-22 result (NVK **14,076** / NVIDIA **12,925**,
**1.09x**) remains the valid cross-driver comparison.

## Workload and result

Unmodified CachyOS package `vkmark 1:2025.01-2.1`, native 64-bit Vulkan (NVK),
not DXVK, OpenGL, Zink, or a game benchmark. Same command and settings as the
2026-10-22 run: Wayland, 13 default scenes, 10 seconds per scene, 800x600,
`B8G8R8A8Unorm`, immediate present mode. Three measured runs, each preceded by
`cube:duration=3`; layers disabled in the benchmark child processes only.

```sh
python3 docs/benchmarks/run-vkmark.py nvgd \
  docs/benchmarks/vulkan-2026-10-23/nvgd --runs 3
```

| Run | NVK score |
|---|---:|
| 1 | 15,510 |
| 2 | 15,549 |
| 3 | 15,497 |
| **Median** | **15,510** |

Range: **15,497–15,549**. The runner verified the same vkmark binary, plugin and
data SHA-256 hashes as the 2026-10-22 NVK run (47 files). The score is the median
of vkmark's reported aggregate scores; it is not reconstructed from the
per-scene medians below.

## Tested system

- NVIDIA GeForce RTX 3050 6 GB, GA107, PCI `10de:2584`.
- Intel Xeon E5-2697 v3; `schedutil` governor.
- CachyOS kernel `7.3.0-3-cachyos`; `nvgd-dkms 1.0.0-20`, GSP `615.78.08`.
- Mesa `1:27.0.0_devel.r231961.ga1c7b37fcb7b-1` (`a1c7b37fcb7b`), NVK.
- vkmark `1:2025.01-2.1`, Assimp `6.0.5-1.1`, GNOME Shell `1:51.1-1.1`,
  Mutter `51.1-1`, Wayland desktop.
- Load before the suite: `3.34 3.60 3.28`; normal desktop/services remained
  active; no clock, CPU-affinity or power-policy changes were made.
- No driver overrides (`NVGD_*`, `NVK_*`, `NAK_*`, `MESA_*`, `__GL_*`, `__VK_*`).
- Sampled GPU clock: mean **1,922 MHz**, peak **2,025 MHz**; mean power **61.1 W**,
  peak temperature **71.6°C**. Samples include startup and scene transitions;
  they are not per-scene GPU timings.

## Per-scene medians (FPS, three runs)

The 2026-10-22 NVK column is the previous NVK measurement, shown only to
quantify this re-measurement. It is **not** an NVIDIA comparison.

| Scene | 2026-10-22 NVK | 2026-10-23 NVK | Change |
|---|---:|---:|---:|
| vertex, device-local=true | 15,547 | 17,442 | 1.12x |
| vertex, device-local=false | 15,325 | 17,701 | 1.16x |
| texture, anisotropy=0 | 12,467 | 15,488 | 1.24x |
| texture, anisotropy=16 | 12,916 | 14,813 | 1.15x |
| shading, gouraud | 12,569 | 15,824 | 1.26x |
| shading, blinn-phong-inf | 13,122 | 15,381 | 1.17x |
| shading, phong | 12,280 | 14,549 | 1.18x |
| shading, cel | 13,241 | 15,035 | 1.14x |
| effect2d, edge | 16,250 | 18,409 | 1.13x |
| effect2d, blur | 9,182 | 9,538 | 1.04x |
| desktop | 9,529 | 9,284 | 0.97x |
| cube | 19,285 | 20,444 | 1.06x |
| clear | 18,690 | 17,879 | 0.96x |

The aggregate score changed from **14,076** to **15,510** (**+10.2%**), but the
per-scene changes are not uniform: the 3D scenes rose 12–26%, blur rose 4%, and
desktop/clear were 3–4% lower. This is a re-measurement of the current stack,
not evidence of a driver-code change.

## Limitations

- **NVK only.** NVIDIA was not re-run on 2026-10-23. Do not recompute the
  NVK/NVIDIA ratio from 15,510; the paired 2026-10-22 result remains **1.09x**.
- The system state differs from 2026-10-22 (background load, thermals,
  scheduling). The higher aggregate score is not attributable to a code change.
- vkmark `FrameTime` is reciprocal FPS, not input latency or a p95/p99
  frametime distribution.
- The stock vkmark semaphore-reuse validation VUIDs remain; these timings are
  not a Vulkan correctness certification.

## Raw evidence

- [Environment, package versions and binary/data hashes](nvgd/environment.json)
- [Run 1](nvgd/run-1.log), [run 2](nvgd/run-2.log), [run 3](nvgd/run-3.log)
- [Summary](nvgd/summary.json)
- GPU telemetry: [run 1](nvgd/run-1-telemetry.csv),
  [run 2](nvgd/run-2-telemetry.csv), [run 3](nvgd/run-3-telemetry.csv)
- Warm-ups: [1](nvgd/warmup-1.log), [2](nvgd/warmup-2.log),
  [3](nvgd/warmup-3.log)

## Reproducing a paired run

From the nvgd/Mesa repository root, after rebooting into NVIDIA with the module
loaded:

```sh
python3 docs/benchmarks/run-vkmark.py nvidia \
  docs/benchmarks/vulkan-2026-10-23/nvidia --runs 3 \
  --reference docs/benchmarks/vulkan-2026-10-23/nvgd/environment.json
```

This uses the 2026-10-23 NVK environment to verify the vkmark binary/plugin/data
hashes. Do not compare the resulting NVIDIA score to the 2026-10-22 NVK score;
use the new NVK run in this directory for a fresh pair.
