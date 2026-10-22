#include "nouveau_vk_video_h264.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "util/vl_zscan_data.h"

bool
nouveau_vk_h264_append(struct nouveau_vk_h264_bitstream *stream,
                      unsigned count, const void *const *buffers,
                      const unsigned *sizes)
{
   if (!stream || (count && (!buffers || !sizes)) ||
       stream->size > NOUVEAU_VK_H264_MAX_BITSTREAM)
      return false;

   size_t total = stream->size;
   for (unsigned i = 0; i < count; i++) {
      if ((sizes[i] && !buffers[i]) ||
          sizes[i] > NOUVEAU_VK_H264_MAX_BITSTREAM - total)
         return false;
      total += sizes[i];
   }
   if (total > stream->capacity) {
       /* Grow geometrically: appending many slices or gradually larger
        * frames should not reallocate the whole accumulated bitstream.
        */
       size_t capacity = stream->capacity > 4096 ? stream->capacity : 4096;
       while (capacity < total)
          capacity *= 2;
       if (capacity > NOUVEAU_VK_H264_MAX_BITSTREAM)
          capacity = NOUVEAU_VK_H264_MAX_BITSTREAM;
      uint8_t *data = realloc(stream->data, capacity);
      if (!data)
         return false;
      stream->data = data;
      stream->capacity = capacity;
   }
   for (unsigned i = 0; i < count; i++) {
      if (sizes[i])
         memcpy(stream->data + stream->size, buffers[i], sizes[i]);
      stream->size += sizes[i];
   }
   return true;
}

struct rbsp_reader {
   const uint8_t *next;
   const uint8_t *end;
   unsigned zeros;
   unsigned bits;
   uint8_t byte;
   bool failed;
};

static uint32_t
read_bits(struct rbsp_reader *r, unsigned count)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < count; i++) {
      if (!r->bits) {
         if (r->next == r->end) {
            r->failed = true;
            return 0;
         }
         uint8_t byte = *r->next++;
         if (r->zeros >= 2 && byte == 3) {
            if (r->next == r->end || *r->next > 3) {
               r->failed = true;
               return 0;
            }
            r->zeros = 0;
            byte = *r->next++;
         }
         r->zeros = byte == 0 ? r->zeros + 1 : 0;
         r->byte = byte;
         r->bits = 8;
      }
      value = (value << 1) | ((r->byte >> --r->bits) & 1);
   }
   return value;
}

static uint32_t
read_ue(struct rbsp_reader *r)
{
   unsigned zeros = 0;
   while (!read_bits(r, 1)) {
      if (r->failed || ++zeros > 31) {
         r->failed = true;
         return 0;
      }
   }
   return ((1u << zeros) - 1) + read_bits(r, zeros);
}

static bool
next_nal(const uint8_t *data, size_t size, size_t begin,
         size_t *start, size_t *payload)
{
   for (size_t i = begin; i + 3 < size; i++) {
      if (data[i] || data[i + 1])
         continue;
      if (data[i + 2] == 1) {
         *start = i;
         *payload = i + 3;
         return true;
      }
      if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) {
         *start = i;
         *payload = i + 4;
         return true;
      }
   }
   return false;
}

bool
nouveau_vk_h264_index(struct nouveau_vk_h264_bitstream *stream,
                     const struct pipe_h264_picture_desc *picture)
{
   if (!stream || !picture || !picture->pps || !picture->pps->sps ||
       !picture->pps->sps->frame_mbs_only_flag || picture->field_pic_flag ||
       picture->pps->sps->separate_colour_plane_flag ||
       picture->pps->sps->log2_max_frame_num_minus4 > 12 ||
       picture->pps->sps->pic_width_in_mbs_minus1 > 255 ||
       picture->pps->sps->pic_height_in_mbs_minus1 > 255 ||
       !stream->data || !stream->size || stream->size > NOUVEAU_VK_H264_MAX_BITSTREAM)
      return false;

   stream->slice_count = 0;
   stream->intra = true;
   size_t start, payload;
   if (!next_nal(stream->data, stream->size, 0, &start, &payload))
      return false;
   do {
      size_t following, next_payload;
      bool more = next_nal(stream->data, stream->size, payload + 1,
                           &following, &next_payload);
      size_t end = more ? following : stream->size;
      uint8_t nal = stream->data[payload];
      unsigned type = nal & 31;
      if ((nal & 0x80) || (type >= 2 && type <= 4))
         return false;
      if (type == 1 || type == 5) {
         if (stream->slice_count == NOUVEAU_VK_H264_MAX_SLICES)
            return false;
         struct rbsp_reader r = {
            .next = stream->data + payload + 1,
            .end = stream->data + end,
         };
         uint32_t first_mb = read_ue(&r);
         uint32_t slice_type = read_ue(&r);
         uint32_t pps_id = read_ue(&r);
         uint32_t frame_num = read_bits(&r, picture->pps->sps->log2_max_frame_num_minus4 + 4);
         uint32_t idr_pic_id = type == 5 ? read_ue(&r) : 0;
         uint64_t mb_count = ((uint64_t)picture->pps->sps->pic_width_in_mbs_minus1 + 1) *
                             ((uint64_t)picture->pps->sps->pic_height_in_mbs_minus1 + 1);
         if (r.failed || first_mb >= mb_count || slice_type > 9 || slice_type % 5 > 2 || pps_id > 255 ||
             idr_pic_id > UINT16_MAX || frame_num != picture->frame_num)
            return false;
         if (stream->slice_count == 0) {
            stream->pps_id = pps_id;
            stream->frame_num = frame_num;
            stream->idr_pic_id = idr_pic_id;
            stream->idr = type == 5;
         } else if (stream->pps_id != pps_id || stream->idr != (type == 5) ||
                    stream->idr_pic_id != idr_pic_id) {
            return false;
         }
         stream->intra &= slice_type % 5 == 2;
         stream->slice_offsets[stream->slice_count++] = start;
      }
      if (!more)
         break;
      start = following;
      payload = next_payload;
   } while (true);
   return stream->slice_count && stream->slice_count == picture->slice_count;
}

bool
nouveau_vk_h264_translate(const struct pipe_h264_picture_desc *picture,
                         const struct nouveau_vk_h264_bitstream *stream,
                         StdVideoH264ProfileIdc profile, StdVideoH264LevelIdc level,
                         struct nouveau_vk_h264_parameters *out)
{
   if (!picture || !picture->pps || !picture->pps->sps || !stream || !out)
      return false;
   const struct pipe_h264_pps *pps = picture->pps;
   const struct pipe_h264_sps *sps = pps->sps;
   if (picture->base.protected_playback || picture->field_pic_flag ||
       !sps->frame_mbs_only_flag || sps->mb_adaptive_frame_field_flag ||
       sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag ||
       sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8 ||
       sps->log2_max_frame_num_minus4 > 12 || sps->pic_order_cnt_type > 2 ||
       sps->log2_max_pic_order_cnt_lsb_minus4 > 12 || pps->num_slice_groups_minus1 ||
       pps->weighted_bipred_idc > 2 || picture->num_ref_frames > 16 ||
       pps->pic_init_qp_minus26 < -26 || pps->pic_init_qp_minus26 > 25 ||
       pps->pic_init_qs_minus26 < -26 || pps->pic_init_qs_minus26 > 25 ||
       pps->chroma_qp_index_offset < -12 || pps->chroma_qp_index_offset > 12 ||
       pps->second_chroma_qp_index_offset < -12 || pps->second_chroma_qp_index_offset > 12 ||
       picture->num_ref_idx_l0_active_minus1 > 15 ||
       picture->num_ref_idx_l1_active_minus1 > 15 ||
       picture->frame_num >= (1u << (sps->log2_max_frame_num_minus4 + 4)) ||
       stream->frame_num != picture->frame_num ||
       !stream->slice_count || stream->slice_count != picture->slice_count ||
       stream->slice_count > NOUVEAU_VK_H264_MAX_SLICES ||
       sps->pic_width_in_mbs_minus1 > 255 || sps->pic_height_in_mbs_minus1 > 255 ||
       (profile != STD_VIDEO_H264_PROFILE_IDC_BASELINE &&
        profile != STD_VIDEO_H264_PROFILE_IDC_MAIN && profile != STD_VIDEO_H264_PROFILE_IDC_HIGH) ||
       level < STD_VIDEO_H264_LEVEL_IDC_1_0 || level > STD_VIDEO_H264_LEVEL_IDC_5_2)
      return false;

   memset(out, 0, sizeof(*out));
   out->sps = (StdVideoH264SequenceParameterSet) {
      .flags = {
         .direct_8x8_inference_flag = sps->direct_8x8_inference_flag,
         .frame_mbs_only_flag = 1,
         .delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag,
         .gaps_in_frame_num_value_allowed_flag = sps->gaps_in_frame_num_value_allowed_flag,
      },
      .profile_idc = profile,
      .level_idc = level,
      .chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420,
      .log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4,
      .pic_order_cnt_type = sps->pic_order_cnt_type,
      .offset_for_non_ref_pic = sps->offset_for_non_ref_pic,
      .offset_for_top_to_bottom_field = sps->offset_for_top_to_bottom_field,
      .log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4,
      .num_ref_frames_in_pic_order_cnt_cycle = sps->num_ref_frames_in_pic_order_cnt_cycle,
      .max_num_ref_frames = picture->num_ref_frames,
      .pic_width_in_mbs_minus1 = sps->pic_width_in_mbs_minus1,
      .pic_height_in_map_units_minus1 = sps->pic_height_in_mbs_minus1,
      .pOffsetForRefFrame = out->poc_offsets,
   };
   memcpy(out->poc_offsets, sps->offset_for_ref_frame, sizeof(out->poc_offsets));
   out->pps = (StdVideoH264PictureParameterSet) {
      .flags = {
         .transform_8x8_mode_flag = pps->transform_8x8_mode_flag,
         .redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag,
         .constrained_intra_pred_flag = pps->constrained_intra_pred_flag,
         .deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag,
         .weighted_pred_flag = pps->weighted_pred_flag,
         .bottom_field_pic_order_in_frame_present_flag = pps->bottom_field_pic_order_in_frame_present_flag,
         .entropy_coding_mode_flag = pps->entropy_coding_mode_flag,
         .pic_scaling_matrix_present_flag = 1,
      },
      .pic_parameter_set_id = stream->pps_id,
      .num_ref_idx_l0_default_active_minus1 = picture->num_ref_idx_l0_active_minus1,
      .num_ref_idx_l1_default_active_minus1 = picture->num_ref_idx_l1_active_minus1,
      .weighted_bipred_idc = pps->weighted_bipred_idc,
      .pic_init_qp_minus26 = pps->pic_init_qp_minus26,
      .pic_init_qs_minus26 = pps->pic_init_qs_minus26,
      .chroma_qp_index_offset = pps->chroma_qp_index_offset,
      .second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset,
      .pScalingLists = &out->scaling,
   };
   out->scaling.scaling_list_present_mask = pps->transform_8x8_mode_flag ? 0xff : 0x3f;
   for (unsigned m = 0; m < 6; m++) {
      for (unsigned i = 0; i < 16; i++) {
         uint8_t value = pps->ScalingList4x4[m][vl_zscan_normal_16[i]];
         if (!value)
            return false;
         out->scaling.ScalingList4x4[m][i] = value;
      }
   }
   if (pps->transform_8x8_mode_flag) {
      for (unsigned m = 0; m < 2; m++) {
         for (unsigned i = 0; i < 64; i++) {
            uint8_t value = pps->ScalingList8x8[m][vl_zscan_normal[i]];
            if (!value)
               return false;
            out->scaling.ScalingList8x8[m][i] = value;
         }
      }
   }
   out->picture = (StdVideoDecodeH264PictureInfo) {
      .flags = {
         .is_intra = stream->intra,
         .IdrPicFlag = stream->idr,
         .is_reference = picture->is_reference,
      },
      .pic_parameter_set_id = stream->pps_id,
      .frame_num = stream->frame_num,
      .idr_pic_id = stream->idr_pic_id,
      .PicOrderCnt = {picture->field_order_cnt[0], picture->field_order_cnt[1]},
   };
   return true;
}

bool
nouveau_vk_h264_reference(const struct pipe_h264_picture_desc *picture,
                         unsigned index, StdVideoDecodeH264ReferenceInfo *out)
{
   if (!picture || !out || index >= 16 || !picture->ref[index] ||
       picture->is_non_existing[index] || !picture->top_is_reference[index] ||
       !picture->bottom_is_reference[index] || picture->frame_num_list[index] > UINT16_MAX)
      return false;
   *out = (StdVideoDecodeH264ReferenceInfo) {
      .flags.used_for_long_term_reference = picture->is_long_term[index],
      .FrameNum = picture->frame_num_list[index],
   };
   memcpy(out->PicOrderCnt, picture->field_order_cnt_list[index], sizeof(out->PicOrderCnt));
   return true;
}
