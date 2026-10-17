#ifndef NOUVEAU_NVENC_H264_H
#define NOUVEAU_NVENC_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NV_NVENC_7_3 1
#include "nvenc_drv.h"

#define NOUVEAU_NVENC_H264_PICTURE_RESERVED_BYTES 1024u

struct nouveau_nvenc_h264_config {
   nvenc_h264_drv_pic_setup_s picture;
   uint8_t picture_padding[NOUVEAU_NVENC_H264_PICTURE_RESERVED_BYTES -
                           sizeof(nvenc_h264_drv_pic_setup_s)];
   _Alignas(64) nvenc_h264_slice_control_s slice;
   _Alignas(64) nvenc_h264_me_control_s me;
   _Alignas(64) nvenc_h264_md_control_s md;
   _Alignas(64) nvenc_h264_quant_control_s quant;
   _Alignas(64) nvenc_pred_weight_table_s weights;
};

unsigned nouveau_nvenc_h264_level(unsigned width, unsigned height,
                                  unsigned frame_rate_num, unsigned frame_rate_den);
bool nouveau_nvenc_h264_idr_config(struct nouveau_nvenc_h264_config *config,
                                  unsigned width, unsigned height, unsigned qp,
                                  unsigned bitstream_bytes, unsigned history_bytes,
                                  bool force_ipcm);
bool nouveau_nvenc_h264_idr_feedback(const nvenc_pic_stat_s *feedback,
                                    bool completion_signalled, uint32_t picture_index,
                                    unsigned bitstream_bytes, unsigned *output_bytes);
bool nouveau_nvenc_h264_idr_headers(const struct nouveau_nvenc_h264_config *config,
                                   uint8_t *data, size_t capacity, size_t *size);

#endif
