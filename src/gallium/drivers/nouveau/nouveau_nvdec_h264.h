/* SPDX-License-Identifier: MIT */
#ifndef NOUVEAU_NVDEC_H264_H
#define NOUVEAU_NVDEC_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pipe/p_video_state.h"

#define NOUVEAU_NVDEC_MAX_SLICES 128
#define NOUVEAU_NVDEC_MAX_BITSTREAM (16u * 1024u * 1024u)

struct nouveau_nvdec_bitstream {
   uint8_t *data;
   size_t size, capacity;
   uint32_t offsets[NOUVEAU_NVDEC_MAX_SLICES + 1];
   unsigned slices;
};

struct _nvdec_h264_pic_s;

bool nouveau_nvdec_bitstream_append(struct nouveau_nvdec_bitstream *stream,
                                   unsigned count, const void *const *data,
                                   const unsigned *sizes);
bool nouveau_nvdec_h264_picture(const struct pipe_h264_picture_desc *picture,
                                struct nouveau_nvdec_bitstream *stream,
                                unsigned width, unsigned height,
                                unsigned pitch_y, unsigned pitch_uv,
                                unsigned current, const uint8_t refs[16],
                                struct _nvdec_h264_pic_s *out);

#endif
