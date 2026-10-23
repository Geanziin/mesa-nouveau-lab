# Native video: Gallium and NVK are independent frontends

The nvgd multimedia architecture follows the same separation as Mesa's AMD
drivers: Gallium video and Vulkan Video submit to hardware independently.
The NVIDIA firmware structures, command methods and supported capabilities
are not interchangeable with AMD VCN.

```text
VA-API -> Gallium nvc0 -> native NVDEC / NVENC backend -> nvgd DRM -> video engines
Vulkan Video -> NVK ---------------------------------> nvgd DRM -> NVDEC
```

The former `nouveau_vk_video` VA-API-to-Vulkan bridge is removed, not merely
disabled by an environment variable. Gallium decode does not create Vulkan
instances, devices, sessions, images or command buffers. Neither Gallium
capabilities nor its build depend on the NVK Vulkan Video extension list.
NAK remains enabled for OpenGL independently of building the Vulkan driver.
NVK's existing Vulkan Video frontend is retained; Vulkan applications continue
to use it directly.

## Implemented and validated first stage

- **Hardware:** GA107 (the tested RTX 3050 6 GB), nvgd DRM and GSP 615.78.08.
- **Decode:** progressive H.264 Baseline/Main/High, 8-bit 4:2:0, NV12 surfaces.
  The current capability range is 48x16 through 4096x4096; tests cover small,
  cropped, 1080p and 3840x2160 streams, not every size in that range.
- Native NVDEC channel, firmware picture/DPB/scaling parameters and GEM
  surfaces. Decoded planes remain Gallium resources, with no CPU frame
  readback/re-upload inside the decoder.
- B-frames, up to 16 reference pictures, multiple slices and asymmetric
  scaling matrices are covered by pixel comparisons against software decode.
- **Encode:** the existing native H.264 NVENC backend remains. Zero-copy
  input now accepts luma/chroma in separate Gallium GEM objects, as well as
  planes in one object. Functional checks cover Baseline/Main/High and 1080p.
  The existing 1920x1088 encoder limit is unchanged.
- Channel object destruction uses the DRM file descriptor, not the channel
  handle. Recreating native decode contexts no longer fails with `EEXIST`.

There is no claim of AMD feature parity or a codec performance improvement.
The first decoder completes each frame synchronously, with a bounded wait
and the firmware watchdog enabled. Unsupported bit depths, interlaced
pictures, protected playback, layouts and codecs are rejected; they are not
silently sent through Vulkan or decoded on the CPU by this backend.

## Reproduce the independence and correctness checks

Build a separate Gallium driver with **no Vulkan drivers**:

```sh
meson setup /tmp/opencode/nvgd-native-video-only \
  -Dgallium-drivers=nouveau -Dvulkan-drivers=[] \
  -Dgallium-va=enabled -Dnouveau-nvenc=true \
  -Dvideo-codecs=h264dec,h264enc -Dllvm=disabled \
  -Dbuild-tests=true -Dbuildtype=debugoptimized -Db_ndebug=false
ninja -C /tmp/opencode/nvgd-native-video-only \
  src/gallium/targets/dri/libgallium-27.0.0-devel.so \
  src/gallium/targets/dri/nvgd_drv_video.so \
  src/gallium/drivers/nouveau/nouveau_nvdec_h264_test
meson test -C /tmp/opencode/nvgd-native-video-only nouveau_nvdec_h264
python3 docs/benchmarks/check-native-video.py /path/to/new-results \
  --driver-dir /tmp/opencode/nvgd-native-video-only/src/gallium/targets/dri
```

The hardware check needs FFmpeg/libx264, ffprobe, vainfo and strace. It does
not install drivers. It forces hardware output followed by `hwdownload`,
compares all 200 decoded frames byte-for-byte to CPU references, and checks
72 encoded frames for valid H.264, expected dimensions/count and direct
Gallium-plane submission. Encode checks are not visual-quality benchmarks.
Both Vulkan ICD variables deliberately point to a nonexistent file. The trace
must show DRM pushbuffer submissions without opening `libvulkan_nouveau.so`.
The report records binary, input, decoded-frame and relevant source hashes.

Recorded on 2026-10-23: [report and hashes](benchmarks/native-video-2026-10-23/report.json),
[native DRM trace](benchmarks/native-video-2026-10-23/native.trace),
[capabilities](benchmarks/native-video-2026-10-23/vainfo.log),
[1080p encode log](benchmarks/native-video-2026-10-23/encode-high-1920x1080.log).
All eight decode and three encode cases passed using the Vulkan-free build.

The C parameter tests exercise raster scaling matrices, stable colocated-MV
indices, DPB markings, slice offsets, input bounds and unsupported-picture
rejection. They were additionally run with AddressSanitizer/UBSan and
`-Werror`. The existing NV12 upload/bitstream-growth checks cover 144
edge/pitch combinations and also pass with both sanitizers.

## Next codecs, in priority order

1. **HEVC Main/Main10 decode:** video/streaming content, including P010 and
   10-bit references. Needs HEVC-specific firmware parameters, reference
   lists, scaling/tile/filter buffers and frame validation.
2. **VP9 Profile 0/2 decode:** browser/web-video workloads. Needs probability
   context, segmentation and reference-state handling, including 10-bit tests.
3. **AV1 Main decode:** web video and newer streaming content. Needs tile,
   CDF, reference, restoration and film-grain handling.

These are implementation stages, **not advertised capabilities**. Each must
pass native hardware output checks before enabling its VA-API profiles.
Encoding additional codecs is a separate task. In particular, GA107 has no
AV1 hardware encoder, so AV1 encode cannot be promised through any API.
