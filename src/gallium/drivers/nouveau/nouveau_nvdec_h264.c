/* SPDX-License-Identifier: MIT
 * Native Gallium H.264 parameters for the NVDEC firmware interface. No Vulkan
 * objects, Vulkan codec structures, loader, or NVK entrypoints are involved.
 */
#include "nouveau_nvdec_h264.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "nvdec_drv.h"
#include "util/u_math.h"

bool
nouveau_nvdec_bitstream_append(struct nouveau_nvdec_bitstream *stream,
                               unsigned count, const void *const *data,
                               const unsigned *sizes)
{
   if (!stream || (count && (!data || !sizes)) ||
       stream->size > NOUVEAU_NVDEC_MAX_BITSTREAM)
      return false;

   size_t total = stream->size;
   for (unsigned i = 0; i < count; i++) {
      if ((sizes[i] && !data[i]) || sizes[i] > NOUVEAU_NVDEC_MAX_BITSTREAM - total)
         return false;
      total += sizes[i];
   }
   if (total > stream->capacity) {
      size_t capacity = MAX2(stream->capacity, 4096);
      while (capacity < total)
         capacity *= 2;
      capacity = MIN2(capacity, NOUVEAU_NVDEC_MAX_BITSTREAM);
      void *allocation = realloc(stream->data, capacity);
      if (!allocation)
         return false;
      stream->data = allocation;
      stream->capacity = capacity;
   }
   for (unsigned i = 0; i < count; i++) {
      if (sizes[i])
         memcpy(stream->data + stream->size, data[i], sizes[i]);
      stream->size += sizes[i];
   }
   return true;
}

static bool
index_slices(struct nouveau_nvdec_bitstream *stream, unsigned expected)
{
   if (!stream || !stream->data || !stream->size ||
       stream->size > NOUVEAU_NVDEC_MAX_BITSTREAM ||
       !expected || expected > NOUVEAU_NVDEC_MAX_SLICES)
      return false;

   stream->slices = 0;
   bool found_nal = false;
   for (size_t i = 0; i + 3 < stream->size; i++) {
      if (stream->data[i] || stream->data[i + 1])
         continue;
      unsigned prefix;
      if (stream->data[i + 2] == 1)
         prefix = 3;
      else if (i + 4 < stream->size && !stream->data[i + 2] && stream->data[i + 3] == 1)
         prefix = 4;
      else
         continue;
      unsigned header = stream->data[i + prefix];
      unsigned type = header & 31;
      if (header & 0x80 || (type >= 2 && type <= 4))
         return false;
      found_nal = true;
      if (type == 1 || type == 5) {
         if (stream->slices == NOUVEAU_NVDEC_MAX_SLICES)
            return false;
         stream->offsets[stream->slices++] = i;
      }
      i += prefix;
   }
   stream->offsets[stream->slices] = stream->size;
   return found_nal && stream->slices == expected;
}

bool
nouveau_nvdec_h264_picture(const struct pipe_h264_picture_desc *picture,
                            struct nouveau_nvdec_bitstream *stream,
                            unsigned width, unsigned height,
                            unsigned pitch_y, unsigned pitch_uv,
                            unsigned current, const uint8_t refs[16],
                            nvdec_h264_pic_s *out)
{
   if (!picture || !picture->pps || !picture->pps->sps || !out || !refs)
      return false;
   const struct pipe_h264_pps *pps = picture->pps;
   const struct pipe_h264_sps *sps = pps->sps;
   /* Progressive 8-bit 4:2:0 is the validated initial backend. Do not silently
    * submit another layout/profile or advertise it through the capabilities.
    */
   if (picture->base.protected_playback || picture->field_pic_flag ||
       !sps->frame_mbs_only_flag || sps->mb_adaptive_frame_field_flag ||
       sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag ||
       sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8 ||
       width < 48 || width > 4096 || height < 16 || height > 4096 ||
       width % 16 || height % 16 || current > 16 ||
       (sps->pic_width_in_mbs_minus1 + 1u) * 16 != width ||
       (sps->pic_height_in_mbs_minus1 + 1u) * 16 != height ||
       pitch_y < width || pitch_uv < width || pitch_y % 64 || pitch_uv % 64 ||
       sps->log2_max_frame_num_minus4 > 12 || sps->pic_order_cnt_type > 2 ||
       sps->log2_max_pic_order_cnt_lsb_minus4 > 12 || pps->num_slice_groups_minus1 ||
       pps->weighted_bipred_idc > 2 || picture->num_ref_frames > 16 ||
       pps->pic_init_qp_minus26 < -26 || pps->pic_init_qp_minus26 > 25 ||
       pps->chroma_qp_index_offset < -12 || pps->chroma_qp_index_offset > 12 ||
       pps->second_chroma_qp_index_offset < -12 || pps->second_chroma_qp_index_offset > 12 ||
       picture->num_ref_idx_l0_active_minus1 > 15 ||
       picture->num_ref_idx_l1_active_minus1 > 15 ||
       picture->frame_num >= (1u << (sps->log2_max_frame_num_minus4 + 4)) ||
       !index_slices(stream, picture->slice_count))
      return false;

   memset(out, 0, sizeof(*out));
   static const uint8_t eos[16] = {0, 0, 1, 0xb, 0, 0, 0, 0,
                                 0, 0, 1, 0xb, 0, 0, 0, 0};
   memcpy(out->eos, eos, sizeof(eos));
   out->explicitEOSPresentFlag = 1;
   out->stream_len = stream->size + sizeof(eos);
   out->slice_count = stream->slices;
   out->mbhist_buffer_size = align((width / 16) * 104, 256);
   out->gptimer_timeout_value = 81000000;
   out->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
   out->delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag;
   out->frame_mbs_only_flag = 1;
   out->PicWidthInMbs = width / 16;
   out->FrameHeightInMbs = height / 16;
   out->tileFormat = 1;
   /* NVC0_RESOURCE_FLAG_VIDEO uses 16-line tiles: two 8-line GOBs. */
   out->gob_height = 0;
   out->entropy_coding_mode_flag = pps->entropy_coding_mode_flag;
   out->pic_order_present_flag = pps->bottom_field_pic_order_in_frame_present_flag;
   out->num_ref_idx_l0_active_minus1 = picture->num_ref_idx_l0_active_minus1;
   out->num_ref_idx_l1_active_minus1 = picture->num_ref_idx_l1_active_minus1;
   out->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
   out->redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag;
   out->transform_8x8_mode_flag = pps->transform_8x8_mode_flag;
   out->pitch_luma = pitch_y;
   out->pitch_chroma = pitch_uv;
   out->luma_bot_offset = width;
   out->chroma_bot_offset = pitch_uv / 2;
   out->HistBufferSize = align((width / 16) * 0x300, 0x200) >> 8;
   out->direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
   out->weighted_pred_flag = pps->weighted_pred_flag;
   out->constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
   out->ref_pic_flag = picture->is_reference;
   out->log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
   out->chroma_format_idc = sps->chroma_format_idc;
   out->pic_order_cnt_type = sps->pic_order_cnt_type;
   out->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
   out->chroma_qp_index_offset = pps->chroma_qp_index_offset;
   out->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
   out->weighted_bipred_idc = pps->weighted_bipred_idc;
   out->CurrPicIdx = current;
   out->CurrColIdx = current;
   out->frame_num = picture->frame_num;
   out->CurrFieldOrderCnt[0] = picture->field_order_cnt[0];
   out->CurrFieldOrderCnt[1] = picture->field_order_cnt[1];

   /* Gallium already supplies the resolved scaling matrices in raster order.
    * Feeding these through StdVideo/zig-zag would add an unnecessary API
    * translation (and risks transposing asymmetric matrices).
    */
   memcpy(out->WeightScale, pps->ScalingList4x4, sizeof(out->WeightScale));
   memcpy(out->WeightScale8x8, pps->ScalingList8x8, sizeof(out->WeightScale8x8));

   for (unsigned i = 0; i < 16; i++) {
      if (!picture->ref[i])
         continue;
      if (refs[i] > 16 || refs[i] == current || picture->is_non_existing[i] ||
          picture->is_field[i] || !picture->top_is_reference[i] ||
          !picture->bottom_is_reference[i] || picture->frame_num_list[i] > UINT16_MAX)
         return false;
      nvdec_dpb_entry_s *ref = &out->dpb[i];
      ref->index = refs[i];
      ref->col_idx = refs[i];
      ref->state = 3;
      ref->is_long_term = picture->is_long_term[i];
      ref->top_field_marking = ref->bottom_field_marking = picture->is_long_term[i] ? 2 : 1;
      memcpy(ref->FieldOrderCnt, picture->field_order_cnt_list[i], sizeof(ref->FieldOrderCnt));
      ref->FrameIdx = picture->frame_num_list[i];
   }
   return true;
}
