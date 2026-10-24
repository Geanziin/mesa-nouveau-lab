/* SPDX-License-Identifier: MIT */
#ifndef NOUVEAU_NVDEC_HEVC_H
#define NOUVEAU_NVDEC_HEVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pipe/p_video_state.h"
#include "nvdec_drv.h"

#define NVDEC_HEVC_SCALING_SIZE 1024
#define NVDEC_HEVC_MAX_TILE_COLUMNS 20
#define NVDEC_HEVC_MAX_TILE_ROWS 22
#define NVDEC_HEVC_SLOTS 16

struct nouveau_nvdec_hevc_frame {
   nvdec_hevc_pic_s pic;
   uint16_t tiles[2 * NVDEC_HEVC_MAX_TILE_COLUMNS * NVDEC_HEVC_MAX_TILE_ROWS];
   uint8_t scaling[NVDEC_HEVC_SCALING_SIZE];
};

unsigned nouveau_nvdec_hevc_filter_size(unsigned width, unsigned height);
unsigned nouveau_nvdec_hevc_coloc_size(unsigned width, unsigned height);

bool nouveau_nvdec_hevc_picture(const struct pipe_h265_picture_desc *picture,
                               const uint8_t *first_nal, size_t nal_size,
                               unsigned pitch_y, unsigned pitch_uv, unsigned bpp,
                               unsigned storage_width, unsigned storage_height,
                               unsigned current, const uint8_t references[16],
                               struct nouveau_nvdec_hevc_frame *out);

#endif
