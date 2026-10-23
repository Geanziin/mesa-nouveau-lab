/* SPDX-License-Identifier: MIT */
#include "nouveau_nvdec_h264.h"
#include "nvdec_drv.h"
#include "pipe/p_video_codec.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
   struct pipe_h264_sps sps = {
      .frame_mbs_only_flag = 1, .chroma_format_idc = 1,
      .pic_width_in_mbs_minus1 = 7, .pic_height_in_mbs_minus1 = 5,
      .direct_8x8_inference_flag = 1,
   };
   struct pipe_h264_pps pps = {.sps = &sps, .transform_8x8_mode_flag = 1};
   /* Asymmetric raster matrices must survive without a zig-zag round trip. */
   for (unsigned m = 0; m < 6; m++)
      for (unsigned i = 0; i < 16; i++)
         pps.ScalingList4x4[m][i] = 16 + m + i;
   for (unsigned m = 0; m < 2; m++)
      for (unsigned i = 0; i < 64; i++)
         pps.ScalingList8x8[m][i] = 17 + m + i;
   struct pipe_h264_picture_desc picture = {
      .pps = &pps, .slice_count = 2, .frame_num = 2,
      .field_order_cnt = {-2, 4}, .is_reference = true,
   };
   const uint8_t data[] = {0, 0, 0, 1, 0x65, 0x80, 0, 0, 1, 0x41, 0x80};
   const void *parts[] = {data, data + 3};
   const unsigned sizes[] = {3, sizeof(data) - 3};
   struct nouveau_nvdec_bitstream stream = {0};
   assert(nouveau_nvdec_bitstream_append(&stream, 2, parts, sizes));
   assert(stream.size == sizeof(data));
   assert(!memcmp(stream.data, data, sizeof(data)));
   uint8_t refs[16];
   memset(refs, 0xff, sizeof(refs));
   nvdec_h264_pic_s hw;
#define TRANSLATE() nouveau_nvdec_h264_picture(&picture, &stream, 128, 96, 128, 128, 16, refs, &hw)
   assert(TRANSLATE());
   assert(stream.slices == 2 && stream.offsets[0] == 0 && stream.offsets[1] == 6);
   assert(stream.offsets[2] == sizeof(data));
   assert(hw.CurrPicIdx == 16 && hw.CurrColIdx == 16 && hw.frame_num == 2);
   assert(hw.CurrFieldOrderCnt[0] == -2 && hw.CurrFieldOrderCnt[1] == 4);
   assert(hw.PicWidthInMbs == 8 && hw.FrameHeightInMbs == 6);
   assert(hw.tileFormat == 1 && hw.gob_height == 0);
   assert(hw.pitch_luma == 128 && hw.pitch_chroma == 128);
   assert(hw.stream_len == sizeof(data) + 16 && hw.explicitEOSPresentFlag);
   assert(!memcmp(hw.WeightScale, pps.ScalingList4x4, sizeof(hw.WeightScale)));
   assert(!memcmp(hw.WeightScale8x8, pps.ScalingList8x8, sizeof(hw.WeightScale8x8)));
   struct pipe_video_buffer reference = {0};
   picture.ref[3] = &reference;
   picture.top_is_reference[3] = picture.bottom_is_reference[3] = true;
   picture.frame_num_list[3] = 1;
   picture.field_order_cnt_list[3][0] = (uint32_t)-4;
   picture.field_order_cnt_list[3][1] = 0;
   assert(!TRANSLATE()); /* Unknown colocated-MV slot must not be guessed. */
   refs[3] = 9;
   assert(TRANSLATE());
   assert(hw.dpb[3].index == 9 && hw.dpb[3].col_idx == 9);
   assert(hw.dpb[3].state == 3 && hw.dpb[3].FrameIdx == 1);
   assert(hw.dpb[3].FieldOrderCnt[0] == (uint32_t)-4);
   picture.is_long_term[3] = true;
   assert(TRANSLATE());
   assert(hw.dpb[3].top_field_marking == 2 && hw.dpb[3].is_long_term);
   refs[3] = 16;
   assert(!TRANSLATE()); /* Cannot decode over a live reference. */
   refs[3] = 9;
   picture.is_non_existing[3] = true;
   assert(!TRANSLATE());
   picture.is_non_existing[3] = false;
   sps.bit_depth_luma_minus8 = 2;
   assert(!TRANSLATE());
   sps.bit_depth_luma_minus8 = 0;
   sps.chroma_format_idc = 3;
   assert(!TRANSLATE());
   sps.chroma_format_idc = 1;
   sps.frame_mbs_only_flag = 0;
   assert(!TRANSLATE());
   sps.frame_mbs_only_flag = 1;
   picture.base.protected_playback = true;
   assert(!TRANSLATE());
   picture.base.protected_playback = false;
   picture.slice_count = 1;
   assert(!TRANSLATE());
   picture.slice_count = 129;
   assert(!TRANSLATE());
   picture.slice_count = 2;
   pps.pic_init_qp_minus26 = 26;
   assert(!TRANSLATE());
   pps.pic_init_qp_minus26 = 0;
   assert(TRANSLATE());
   stream.size = NOUVEAU_NVDEC_MAX_BITSTREAM + 1u;
   assert(!nouveau_nvdec_bitstream_append(&stream, 0, NULL, NULL));
   stream.size = sizeof(data);
   const unsigned oversized = NOUVEAU_NVDEC_MAX_BITSTREAM;
   assert(!nouveau_nvdec_bitstream_append(&stream, 1, parts, &oversized));
   assert(stream.size == sizeof(data));
   assert(!nouveau_nvdec_bitstream_append(&stream, 1, NULL, sizes));
   free(stream.data);
   return 0;
}
