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

## Implemented and validated stages

### Stage 1: H.264

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

### Stage 2: AV1 decode (Profile0)

- **Decode:** AV1 Profile0 8-bit and 10-bit 4:2:0 (NV12 and P010 surfaces),
  128x128 through 4096x4096. Validated bit-exact against libdav1d on:
  all-intra, inter with GOP 24, hidden/altref frames with 16-frame lag and
  `show_existing_frame` handling, 2x2 tile grids, and Main10 10-bit streams.
- Per-surface CDF (adaptation state), segment-ID and temporal-MV buffers;
  default CDF tables are packed into the NVDEC layout at runtime. Global
  motion shear parameters (spec 7.11.3.6), superres steps (7.16), motion
  field projection order and segmentation signs follow the AV1 spec.
- **Not yet covered, rejected explicitly:** film grain (`apply_grain`),
  4:2:2/4:4:4, 12-bit, monochrome, large-scale tiles and profile 1. These
  fail the picture check loudly; there is no silent CPU or Vulkan fallback.

### Stage 3: HEVC decode (Main / Main10)

- **Decode:** HEVC Main 8-bit and Main10 10-bit 4:2:0 (NV12 and P010
  surfaces), 48x16 through 4096x4096, one picture slot per surface with a
  per-picture co-located-MV area. Validated bit-exact against the software
  decoder on: Main with B-frames, Main10, 2x2 tile grids and 3840x2160.
- The driver parses each slice header's `sw_hdr_skip_length` (the bits the
  engine must skip because the frontend already applied them: output flag,
  POC LSB, the whole short-term reference picture set and the long-term
  part) with the spec's bit reader. Scaling lists are re-packed from
  Gallium's raster order into the engine's transposed layout, including the
  spec defaults when no matrix buffer arrives. Tiles are given in CTBs.
- **Not yet covered, rejected explicitly:** range extensions (transform-skip
  rotation, RDPCM, cross-component prediction, extended precision, high
  precision offsets, chroma QP lists), monochrome, separate colour planes,
  4:2:2/4:4:4 and 12-bit. No silent fallback.

### Stage 4: VP9 decode (Profile0 / Profile2)

- **Decode:** VP9 Profile0 8-bit and Profile2 10-bit 4:2:0, 8-bit through
  8192x8192. Validated bit-exact against libvpx on: inter frames with GOP 24
  and backward adaptation, 4x2 tile grids and Profile2 10-bit streams.
- The driver owns the full VP9 header state machine per the spec (6.1-6.3):
  the uncompressed and compressed headers are parsed with a local boolean
  decoder, the four saved probability contexts are loaded/reset, and after
  each frame the probabilities are backward-adapted (8.4) from the engine's
  counter buffer before being saved again. The probability table is packed
  into the NVDEC layout at runtime; segment maps and column MVs ping-pong.
  Frames that take their size from a reference resolve it before the tile
  syntax is parsed, since the frontend reports the frame size.
- The engine reads reference samples up to the superblock-aligned frame size
  without clamping at the frame edge, so after each frame the decoded
  surface's right and bottom padding repeats the edge (8.5.2.4). Gallium
  video surfaces are 64-aligned for this reason.
- **Not yet covered, rejected explicitly:** `show_existing_frame` (the
  frontend outputs those itself), 4:2:2/4:4:4, 12-bit and profiles 1/3.

There is no claim of AMD feature parity or a codec performance improvement.
Both decoders complete each frame synchronously, with a bounded wait
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

The hardware check needs FFmpeg/libx264/libaom/libdav1d/libx265/libvpx,
ffprobe, vainfo and strace. It does not install drivers. It forces hardware
output followed by `hwdownload`, compares every decoded frame byte-for-byte
to CPU references (620 frames: 200 H.264, 168 AV1, 120 HEVC and 132 VP9),
and checks 72 encoded frames for valid H.264, expected dimensions/count and
direct Gallium-plane submission. Encode checks are not visual-quality
benchmarks.
Both Vulkan ICD variables deliberately point to a nonexistent file. The trace
must show DRM pushbuffer submissions without opening `libvulkan_nouveau.so`.
The report records binary, input, decoded-frame and relevant source hashes.

Recorded on 2026-10-23, H.264 stage: [report and hashes](benchmarks/native-video-2026-10-23/report.json),
[native DRM trace](benchmarks/native-video-2026-10-23/native.trace),
[capabilities](benchmarks/native-video-2026-10-23/vainfo.log),
[1080p encode log](benchmarks/native-video-2026-10-23/encode-high-1920x1080.log).
Recorded on 2026-10-23, AV1 stage: [report and hashes](benchmarks/native-video-av1-2026-10-23/report.json),
[10-bit decode log](benchmarks/native-video-av1-2026-10-23/av1-main10-vaapi.log),
[altref decode log](benchmarks/native-video-av1-2026-10-23/av1-altref-vaapi.log).
Recorded on 2026-10-24, HEVC and VP9 stages: [report and hashes](benchmarks/native-video-codecs-2026-10-24/report.json).
All eight H.264 decode, five AV1 decode, four HEVC decode, three VP9 decode
and three encode cases passed using the Vulkan-free build.

The C parameter tests exercise raster scaling matrices, stable colocated-MV
indices, DPB markings, slice offsets, input bounds and unsupported-picture
rejection. They were additionally run with AddressSanitizer/UBSan and
`-Werror`. The AV1 default-CDF packer is compared byte-for-byte against an
independent MIT-licensed NVDEC layout reference at build-test time.
The existing NV12 upload/bitstream-growth checks cover 144
edge/pitch combinations and also pass with both sanitizers.

## Next, in priority order

1. **AV1 film grain and remaining tools:** the FGS buffer and a separate
   display surface, covering `apply_grain` streams end to end.
2. **HEVC range extensions / SCC coverage** and long-term references.
3. **VP9 12-bit (profile 2)** if a VA surface format for it is agreed.

These are implementation stages, **not advertised capabilities**. Each must
pass native hardware output checks before enabling its VA-API profiles.
Encoding additional codecs is a separate task. In particular, GA107 has no
AV1 hardware encoder, so AV1 encode cannot be promised through any API.
