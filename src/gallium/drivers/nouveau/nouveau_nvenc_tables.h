/*
 * SPDX-License-Identifier: MIT
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 */
#ifndef NOUVEAU_NVENC_TABLES_H
#define NOUVEAU_NVENC_TABLES_H

struct nvenc_overlay_run {
   unsigned offset, length;
   const unsigned char *bytes;
};

extern const struct nvenc_overlay_run nvenc_overlay_runs[];
extern const unsigned nvenc_overlay_run_count;

#endif
