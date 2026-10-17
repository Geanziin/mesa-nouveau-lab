#ifndef NOUVEAU_VK_VIDEO_H264_H
#define NOUVEAU_VK_VIDEO_H264_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "pipe/p_video_state.h"
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>

#define NOUVEAU_VK_H264_MAX_SLICES 128
#define NOUVEAU_VK_H264_MAX_BITSTREAM (16u * 1024u * 1024u)

struct nouveau_vk_h264_parameters {
   StdVideoH264SequenceParameterSet sps;
   StdVideoH264PictureParameterSet pps;
   StdVideoH264ScalingLists scaling;
   StdVideoDecodeH264PictureInfo picture;
   int32_t poc_offsets[256];
};

struct nouveau_vk_h264_bitstream {
   uint8_t *data;
   size_t size;
   size_t capacity;
   uint32_t slice_offsets[NOUVEAU_VK_H264_MAX_SLICES];
   uint32_t slice_count;
   uint8_t pps_id;
   uint16_t frame_num;
   uint16_t idr_pic_id;
   bool idr;
   bool intra;
};

bool nouveau_vk_h264_append(struct nouveau_vk_h264_bitstream *stream,
                           unsigned count, const void *const *buffers,
                           const unsigned *sizes);
bool nouveau_vk_h264_index(struct nouveau_vk_h264_bitstream *stream,
                          const struct pipe_h264_picture_desc *picture);
bool nouveau_vk_h264_translate(const struct pipe_h264_picture_desc *picture,
                              const struct nouveau_vk_h264_bitstream *stream,
                              StdVideoH264ProfileIdc profile,
                              StdVideoH264LevelIdc level,
                              struct nouveau_vk_h264_parameters *parameters);
bool nouveau_vk_h264_reference(const struct pipe_h264_picture_desc *picture,
                              unsigned index,
                              StdVideoDecodeH264ReferenceInfo *reference);

#endif
