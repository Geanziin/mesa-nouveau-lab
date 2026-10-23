# Vulkan comparison: RTX 3050 / GA107

Both drivers measured on **2026-10-22, UTC-03** (2026-10-23 UTC), with a reboot
between them. The NVIDIA side uses driver **615.78.08**. No Vulkan result is
inferred from the older OpenGL measurements.

## Workload and result

Unmodified CachyOS package `vkmark 1:2025.01-2.1`, native **64-bit Vulkan**,
not DXVK, OpenGL, Zink, or a game benchmark.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nouveau_icd.x86_64.json \
VK_LOADER_LAYERS_DISABLE='~all~' \
vkmark --winsys wayland --present-mode immediate --size 800x600 \
  --pixel-format B8G8R8A8Unorm
```

- 13 default scenes, **10 seconds per scene**, approximately 130.6 seconds/run.
- Three measured runs, one at a time; a `cube:duration=3` warm-up precedes each.
- Layers disabled in the benchmark child processes only; no `--debug` in timed runs.
- Existing shader caches retained; this is not a cold compilation benchmark.
- Same resolution, pixel format, presentation mode and benchmark binary/data
  must be used on NVIDIA. The runner records their SHA-256 hashes.

| Run | nvgd / NVK score | NVIDIA score |
|---|---:|---:|
| 1 | 14,076 | 13,399 |
| 2 | 13,401 | 12,925 |
| 3 | 14,090 | 12,792 |
| **Median** | **14,076** | **12,925** |

Ranges: NVK **13,401–14,090**; NVIDIA **12,792–13,399**.
The ratio of median aggregate scores is **1.0891x**, or **8.9% higher for NVK**
in this workload. Three runs on one machine are not a statistical claim about
all Vulkan applications. These are vkmark scores, not game FPS. The `FrameTime`
printed by vkmark is reciprocal FPS, not an input-to-display latency measurement
or a p95/p99 frametime distribution. Do not compare vkmark scores to glmark2 scores.

## Tested system

- NVIDIA GeForce RTX 3050 6 GB, GA107, PCI `10de:2584`.
- Intel Xeon E5-2697 v3; `schedutil` governor.
- NVK boot: CachyOS kernel `7.3.0-3-cachyos`; `nvgd-dkms 1.0.0-20`, GSP `615.78.08`.
- NVIDIA boot: kernel `6.18.55-2-cachyos-lts`, loaded NVIDIA module and
  `nvidia-utils 615.78.08-2`; `nvidia-smi` confirms driver `615.78.08`.
- Mesa 32/64 packages `1:27.0.0_devel.r231961.ga1c7b37fcb7b-1`.
- GNOME Shell `1:51.1-1.1`, Mutter `51.1-1`, Wayland desktop.
- droidge/Unity was closed before the measured runs. Normal desktop/services
  remained active; no clock, CPU-affinity or power-policy changes were made.
- NVK sampled GPU clock: mean **1,930 MHz**, peak **2,032 MHz**; mean power
  **58.2 W**, peak temperature **70.9°C** across the measured runs. Samples
  include startup and scene transitions; they are not per-scene GPU timings.

The runner confirmed **identical benchmark/plugin/data hashes**, commands,
`schedutil` governor, vkmark, Assimp, GNOME and Mutter versions on both boots.
However, the **kernels and desktop background load differ**. Before the NVIDIA
run, desktop indexing was observed; the recorded one-minute system load was
3.50 versus 0.87 before NVK. No unrelated services were stopped. These results
describe the two tested stacks, not an isolated causal measurement of driver
code alone. NVIDIA telemetry includes `nvidia-smi` snapshots; missing hwmon
fields in its CSV files are not zero values or evidence of idle hardware.

## Per-scene medians (FPS, three runs per driver)

The overall score above is the median of vkmark's **reported aggregate scores**;
it is not reconstructed by averaging the per-scene medians below.

| Scene | nvgd / NVK | NVIDIA 615.78.08 | NVK / NVIDIA |
|---|---:|---:|---:|
| vertex, device-local=true | 15,547 | 13,698 | 1.14x |
| vertex, device-local=false | 15,325 | 5,317 | 2.88x |
| texture, anisotropy=0 | 12,467 | 14,049 | 0.89x |
| texture, anisotropy=16 | 12,916 | 14,408 | 0.90x |
| shading, gouraud | 12,569 | 14,650 | 0.86x |
| shading, blinn-phong-inf | 13,122 | 14,832 | 0.88x |
| shading, phong | 12,280 | 14,852 | 0.83x |
| shading, cel | 13,241 | 14,757 | 0.90x |
| effect2d, edge | 16,250 | 14,886 | 1.09x |
| effect2d, blur | 9,182 | 10,462 | 0.88x |
| desktop | 9,529 | 12,127 | 0.79x |
| cube | 19,285 | 14,110 | 1.37x |
| clear | 18,690 | 10,673 | 1.75x |

NVK leads in **5/13** scene medians; NVIDIA leads in **8/13**. The larger NVK
gains in host-visible vertex buffers, cube and clear contribute to its higher
aggregate score; the aggregate must not be presented as a universal Vulkan win.

## Raw evidence

- [Derived comparison and limitations](comparison.json)
- [Environment, package versions and binary/data hashes](nvgd/environment.json)
- [Run 1](nvgd/run-1.log), [run 2](nvgd/run-2.log), [run 3](nvgd/run-3.log)
- [Summary](nvgd/summary.json)
- NVIDIA: [environment and hashes](nvidia/environment.json),
  [run 1](nvidia/run-1.log), [run 2](nvidia/run-2.log), [run 3](nvidia/run-3.log),
  [summary](nvidia/summary.json)
- GPU telemetry: [run 1](nvgd/run-1-telemetry.csv),
  [run 2](nvgd/run-2-telemetry.csv), [run 3](nvgd/run-3-telemetry.csv)
- [Separate debug preflight](diagnostic-wayland.log): confirms the selected
  Wayland plugin, NVK device and `B8G8R8A8Unorm` format. Its score is **not used**.
- [NVIDIA debug preflight](diagnostic-nvidia-wayland.log): same Wayland plugin
  and pixel format; its score is also **not used**.

**Validation limitation:** the separate debug preflight with validation enabled
reports semaphore-reuse VUIDs (`vkQueueSubmit-pSignalSemaphores-00067` and
`vkAcquireNextImageKHR-semaphore-01779`). The stock benchmark was not patched;
these timings must not be represented as a Vulkan correctness certification.
The NVIDIA diagnostic also reports the same semaphore-reuse VUID categories.
All six timed runs completed all 13 scenes with no application error messages,
with validation/overlay layers disabled. Identical workload settings alone do
not eliminate this validation limitation.

## Reproducing the comparison

From the **same machine's GNOME Wayland session**, close games/recorders, retain
the same vkmark package and do not change display/power settings between sides.
Do not switch drivers or load/unload the kernel module inside the benchmark.

From the nvgd/Mesa repository root:

```sh
python3 docs/benchmarks/run-vkmark.py nvidia /path/to/new-nvidia-results \
  --reference docs/benchmarks/vulkan-2026-10-22/nvgd/environment.json
```

The runner requires the NVIDIA module to be loaded, selects `nvidia_icd.json`,
checks binary/plugin/data hashes against this baseline, repeats the same warm-up
and three full runs, and refuses to overwrite existing results. It records the
actual NVIDIA driver/kernel versions. For fresh paired runs, use new output
directories for both drivers and pass the new NVK `environment.json` as the
NVIDIA reference. Compute the ratio from the two median Vulkan scores; do not
copy the OpenGL driver ratio or overwrite the archived measurements here.
