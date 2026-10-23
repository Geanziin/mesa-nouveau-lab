# Vulkan comparison: RTX 3050 / GA107

NVK measured on **2026-10-22, UTC-03** (2026-10-23 UTC).
The proprietary-driver side has **not been measured yet**.
No NVIDIA Vulkan score or speedup is inferred from the older OpenGL results.

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
| 1 | 14,076 | pending |
| 2 | 13,401 | pending |
| 3 | 14,090 | pending |
| **Median** | **14,076** | **pending** |

Range: **13,401–14,090**. These are vkmark scores, not game FPS. The `FrameTime`
printed by vkmark is reciprocal FPS, not an input-to-display latency measurement
or a p95/p99 frametime distribution. Do not compare vkmark scores to glmark2 scores.

## Tested system

- NVIDIA GeForce RTX 3050 6 GB, GA107, PCI `10de:2584`.
- Intel Xeon E5-2697 v3; `schedutil` governor.
- CachyOS kernel `7.3.0-3-cachyos`; `nvgd-dkms 1.0.0-20`, GSP `615.78.08`.
- Mesa 32/64 packages `1:27.0.0_devel.r231961.ga1c7b37fcb7b-1`.
- GNOME Shell `1:51.1-1.1`, Mutter `51.1-1`, Wayland desktop.
- droidge/Unity was closed before the measured runs. Normal desktop/services
  remained active; no clock, CPU-affinity or power-policy changes were made.
- Sampled GPU clock: mean **1,930 MHz**, peak **2,032 MHz**; mean power
  **58.2 W**, peak temperature **70.9°C** across the measured runs. Samples
  include startup and scene transitions; they are not per-scene GPU timings.

## Raw evidence

- [Environment, package versions and binary/data hashes](nvgd/environment.json)
- [Run 1](nvgd/run-1.log), [run 2](nvgd/run-2.log), [run 3](nvgd/run-3.log)
- [Summary](nvgd/summary.json)
- GPU telemetry: [run 1](nvgd/run-1-telemetry.csv),
  [run 2](nvgd/run-2-telemetry.csv), [run 3](nvgd/run-3-telemetry.csv)
- [Separate debug preflight](diagnostic-wayland.log): confirms the selected
  Wayland plugin, NVK device and `B8G8R8A8Unorm` format. Its score is **not used**.

**Validation limitation:** the separate debug preflight with validation enabled
reports semaphore-reuse VUIDs (`vkQueueSubmit-pSignalSemaphores-00067` and
`vkAcquireNextImageKHR-semaphore-01779`). The stock benchmark was not patched;
these timings must not be represented as a Vulkan correctness certification.
The three timed runs completed all scenes with no application error messages,
with validation/overlay layers disabled. Preserve this caveat for the NVIDIA
comparison; identical workload settings alone do not eliminate that limitation.

## Repeat after rebooting into NVIDIA

From the **same machine's GNOME Wayland session**, close games/recorders, retain
the same vkmark package and do not change display/power settings between sides.
Do not switch drivers or load/unload the kernel module inside the benchmark.

From the nvgd/Mesa repository root:

```sh
python3 docs/benchmarks/run-vkmark.py nvidia \
  docs/benchmarks/vulkan-2026-10-22/nvidia \
  --reference docs/benchmarks/vulkan-2026-10-22/nvgd/environment.json
```

The runner requires the NVIDIA module to be loaded, selects `nvidia_icd.json`,
checks binary/plugin/data hashes against this baseline, repeats the same warm-up
and three full runs, and refuses to overwrite existing results. It records the
actual NVIDIA driver/kernel versions, which may differ from the historical
OpenGL comparison. Afterward, fill in the Vulkan row and compute the ratio as
`14,076 / median(NVIDIA scores)`; do not copy the OpenGL driver ratio.
