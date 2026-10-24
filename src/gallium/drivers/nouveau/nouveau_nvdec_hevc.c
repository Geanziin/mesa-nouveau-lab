/* SPDX-License-Identifier: MIT
 * Gallium H.265 descriptors to the documented NVDEC firmware interface.
 * NVDEC submission layout reference: mickys/nvenc, src/hevcdec.c and
 * src/hevcparse.c (MIT, Copyright 2026 mickys); the HEVC default scaling
 * lists there match ITU-T H.265 7.4.5. Reference-picture-set handling,
 * sw_hdr_skip_length counting and QpBdOffset follow H.265 7.3.6 and 8.3.
 */
#include "nouveau_nvdec_hevc.h"
#include "util/u_math.h"
#include "util/vl_rbsp.h"
#include "util/vl_vlc.h"
#include <string.h>

#define NVDEC_ALIGN256(x) align64(x, 256)

/* The six neighbour areas the engine addresses, one after the other: the
 * deblocking top and left lines, the SAO left area at eight times
 * nvdec_drv.h's size (the reference found the 1x size too small with many
 * tile columns), then the BSD control, filter-above, SAO-above and
 * slice-edge areas, sized by the larger picture dimension, generously for
 * 24 tile columns. pic setup offsets are in 256-byte units. */
static unsigned
filter_layout(unsigned width, unsigned height, nvdec_hevc_pic_s *p)
{
   unsigned rows = align(width, 16) / 16 + 2;
   unsigned cols = align(height, 16) / 16 + 2;
   unsigned top = NVDEC_ALIGN256((uint64_t)rows * HEVC_DBLK_TOP_SIZE_IN_SB16);
   unsigned left = NVDEC_ALIGN256((uint64_t)cols * HEVC_DBLK_LEFT_SIZE_IN_SB16);
   unsigned sao_left = NVDEC_ALIGN256((uint64_t)cols * HEVC_SAO_LEFT_SIZE_IN_SB16);
   unsigned dim = MAX2(width, height);
   unsigned offset = NVDEC_ALIGN256((uint64_t)top + left);
   if (p)
      p->HevcSaoBufferOffset = offset >> 8;
   offset += NVDEC_ALIGN256((uint64_t)sao_left * 8);
   if (p)
      p->HevcBsdCtrlOffset = offset >> 8;
   offset += NVDEC_ALIGN256((uint64_t)height * GIP_ASIC_BSD_CTRL_RAM_SIZE * 24);
   unsigned above = offset;
   offset += NVDEC_ALIGN256((uint64_t)dim * GIP_ASIC_VERT_FILTER_RAM_SIZE * 24);
   if (p) {
      p->v1.hevc_main10_444_ext.HevcFltAboveOffset = above >> 8;
      p->v1.hevc_main10_444_ext.HevcSaoAboveOffset = offset >> 8;
   }
   offset += NVDEC_ALIGN256((uint64_t)dim * GIP_ASIC_VERT_SAO_RAM_SIZE * 24);
   if (p)
      p->v3.HevcSliceEdgeOffset = offset >> 8;
   return offset + NVDEC_ALIGN256((uint64_t)width * 64);
}

unsigned
nouveau_nvdec_hevc_filter_size(unsigned width, unsigned height)
{
   return filter_layout(width, height, NULL);
}

unsigned
nouveau_nvdec_hevc_coloc_size(unsigned width, unsigned height)
{
   /* 128 bytes per 16x16 block per picture; the engine indexes one area per
    * picture slot inside the single co-located buffer. */
   return NVDEC_ALIGN256((uint64_t)(width / 16) * (height / 16) * 128);
}

/* H.265 6.5.3 up-right diagonal scan: each diagonal is traversed from the
 * left column towards the top-right corner, as NVDEC reads scaling lists. */
static void
diag_scan(unsigned n, uint8_t *x, uint8_t *y)
{
   unsigned i = 0;
   for (unsigned k = 0; k < 2 * n - 1; k++) {
      for (unsigned yy = MIN2(k, n - 1);; yy--) {
         unsigned xx = k - yy;
         if (xx < n) {
            x[i] = xx;
            y[i] = yy;
            i++;
         }
         if (!yy)
            break;
      }
   }
   assert(i == n * n);
}

static void
pack_scaling_list(uint8_t *dst, const uint8_t *src, unsigned n)
{
   /* Gallium stores each list in raster order; the engine reads it
    * column by column, so element (x, y) goes to x * n + y. */
   for (unsigned y = 0; y < n; y++)
      for (unsigned x = 0; x < n; x++)
         dst[x * n + y] = src[y * n + x];
}

static void
diag_to_raster(const uint8_t *src, uint8_t *dst, unsigned n)
{
   uint8_t xs[64], ys[64];
   diag_scan(n, xs, ys);
   for (unsigned i = 0; i < n * n; i++)
      dst[ys[i] * n + xs[i]] = src[i];
}

/* The spec's default scaling lists (7.4.5), in diag scan order. */
static const uint8_t default_intra4[16] = { 16, 16, 16, 16, 16, 16, 16, 16,
                                            16, 16, 16, 16, 16, 16, 16, 16 };
static const uint8_t default_intra8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
    24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115 };
static const uint8_t default_inter8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91 };

static void
write_scaling(uint8_t *out, const struct pipe_h265_sps *sps)
{
   memset(out, 0, NVDEC_HEVC_SCALING_SIZE);

   const uint8_t *l4[6], *l8[6], *l16[6];
   for (unsigned m = 0; m < 6; m++) {
      l4[m] = sps->ScalingList4x4[m];
      l8[m] = sps->ScalingList8x8[m];
      l16[m] = sps->ScalingList16x16[m];
   }
   const uint8_t *l32[2] = { sps->ScalingList32x32[0], sps->ScalingList32x32[1] };
   unsigned dc16[6], dc32[2];
   for (unsigned m = 0; m < 6; m++)
      dc16[m] = sps->ScalingListDCCoeff16x16[m];
   dc32[0] = sps->ScalingListDCCoeff32x32[0];
   dc32[1] = sps->ScalingListDCCoeff32x32[1];

   /* scaling_list_enabled_flag with no QMatrix buffer means the spec's
    * default lists (7.4.5); Gallium leaves everything zeroed in that case,
    * and a real matrix never is. */
   bool defaults = !l4[0][0] && !l8[0][0] && !l16[0][0] && !l32[0][0] && !dc16[0];
   uint8_t flat4[16], intra8[64], inter8[64];
   memset(flat4, 16, sizeof(flat4));
   if (defaults) {
      diag_to_raster(default_intra8, intra8, 8);
      diag_to_raster(default_inter8, inter8, 8);
   }

   const uint8_t *source[4][6];
   for (unsigned m = 0; m < 6; m++) {
      source[0][m] = defaults ? flat4 : l4[m];
      source[1][m] = defaults ? (m < 3 ? intra8 : inter8) : l8[m];
      source[2][m] = defaults ? (m < 3 ? intra8 : inter8) : l16[m];
   }
   if (defaults) {
      source[3][0] = intra8;
      source[3][3] = inter8;
      for (unsigned m = 0; m < 6; m++)
         dc16[m] = 16;
      dc32[0] = dc32[1] = 16;
   } else {
      source[3][0] = l32[0];
      source[3][3] = l32[1];
   }

   /* 16x16 then 32x32 DC values, 8 bytes padding, then the 4x4, 8x8, 16x16
    * and 32x32 (intra and inter luma) lists, each transposed. */
   for (unsigned m = 0; m < 6; m++)
      out[m] = dc16[m];
   out[6] = dc32[0];
   out[7] = dc32[1];
   uint8_t *q = out + 16;
   for (unsigned size = 0; size < 4; size++) {
      unsigned n = size == 0 ? 4 : 8;
      for (unsigned m = 0; m < 6; m += size == 3 ? 3 : 1) {
         pack_scaling_list(q, source[size][m], n);
         q += n * n;
      }
   }
}

/* Count the slice-header bits the driver has consumed after slice_type():
 * pic_output_flag, colour_plane_id, slice_pic_order_cnt_lsb, the whole
 * short-term reference picture set and the long-term part (H.265 7.3.6).
 * Also reports whether the slice carried its own st_ref_pic_set().
 */
static bool
slice_skip_bits(const struct pipe_h265_picture_desc *pic, const uint8_t *nal,
                size_t size, unsigned *skip, bool *header_rps)
{
   const struct pipe_h265_pps *pps = pic->pps;
   const struct pipe_h265_sps *sps = pps->sps;
   const void *data = nal;
   unsigned bits_size = size;
   struct vl_vlc vlc;
   vl_vlc_init(&vlc, 1, (const void *const *)&data, &bits_size);
   vl_vlc_eatbits(&vlc, 1);
   unsigned type = vl_vlc_get_uimsbf(&vlc, 6);
   vl_vlc_eatbits(&vlc, 9);
   struct vl_rbsp rbsp;
   vl_rbsp_init(&rbsp, &vlc, ~0, true);

   unsigned first = vl_rbsp_u(&rbsp, 1);
   if (type >= 16 && type <= 23)
      vl_rbsp_u(&rbsp, 1); /* no_output_of_prior_pics_flag */
   if (vl_rbsp_ue(&rbsp) > 15) /* slice_pic_parameter_set_id */
      return false;
   if (!first) {
      if (pps->dependent_slice_segments_enabled_flag && vl_rbsp_u(&rbsp, 1))
         return false; /* a dependent segment carries no reference picture set */
      unsigned ctb = 1 << (sps->log2_min_luma_coding_block_size_minus3 + 3 +
                           sps->log2_diff_max_min_luma_coding_block_size);
      unsigned ctbs = DIV_ROUND_UP(sps->pic_width_in_luma_samples, ctb) *
                      DIV_ROUND_UP(sps->pic_height_in_luma_samples, ctb);
      if (ctbs < 2)
         return false;
      vl_rbsp_u(&rbsp, util_logbase2(ctbs - 1) + 1);
   }
   for (unsigned i = 0; i < pps->num_extra_slice_header_bits; i++)
      vl_rbsp_u(&rbsp, 1);
   vl_rbsp_ue(&rbsp); /* slice_type */

   unsigned bits = 0;
   if (pps->output_flag_present_flag)
      bits++;
   if (sps->separate_colour_plane_flag)
      bits += 2;
   if (!pic->IDRPicFlag) {
      bits += sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
      unsigned sps_flag = vl_rbsp_u(&rbsp, 1);
      bits++;
      *header_rps = !sps_flag;
      if (!sps_flag) {
         /* The app counted the st_ref_pic_set() structure; a legal one always
          * has the two num_*_pics syntax elements. */
         if (pic->NumShortTermPictureSliceHeaderBits < 2)
            return false;
         bits += pic->NumShortTermPictureSliceHeaderBits;
      } else if (sps->num_short_term_ref_pic_sets > 1) {
         bits += util_logbase2_ceil(sps->num_short_term_ref_pic_sets);
      }
      if (sps->long_term_ref_pics_present_flag) {
         unsigned lt_sps = 0;
         if (sps->num_long_term_ref_pics_sps > 0) {
            lt_sps = vl_rbsp_ue(&rbsp);
            if (lt_sps > sps->num_long_term_ref_pics_sps)
               return false;
         }
         unsigned lt_pics = vl_rbsp_ue(&rbsp);
         if (lt_sps + lt_pics > 32)
            return false;
         for (unsigned i = 0; i < lt_sps + lt_pics; i++) {
            if (i >= lt_sps) {
               vl_rbsp_u(&rbsp, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
               vl_rbsp_u(&rbsp, 1);
               bits += sps->log2_max_pic_order_cnt_lsb_minus4 + 5;
            } else if (sps->num_long_term_ref_pics_sps > 1) {
               vl_rbsp_u(&rbsp, util_logbase2_ceil(sps->num_long_term_ref_pics_sps));
               bits += util_logbase2_ceil(sps->num_long_term_ref_pics_sps);
            }
            if (vl_rbsp_u(&rbsp, 1)) { /* delta_poc_msb_present_flag */
               unsigned zeros = 0;
               while (!vl_rbsp_u(&rbsp, 1))
                  if (++zeros > 31)
                     return false;
               bits += 2 * (zeros + 1) + 1;
            }
            bits++;
         }
      }
   }
   *skip = bits;
   return true;
}

bool
nouveau_nvdec_hevc_picture(const struct pipe_h265_picture_desc *picture,
                           const uint8_t *first_nal, size_t nal_size,
                           unsigned pitch_y, unsigned pitch_uv, unsigned bpp,
                           unsigned storage_width, unsigned storage_height,
                           unsigned current, const uint8_t references[16],
                           struct nouveau_nvdec_hevc_frame *out)
{
   if (!picture || !out || !first_nal || !references || current >= NVDEC_HEVC_SLOTS)
      return false;
   const struct pipe_h265_pps *pps = picture->pps;
   const struct pipe_h265_sps *sps = pps->sps;
   unsigned bit_depth = sps->bit_depth_luma_minus8 + 8;
   unsigned min_cb = 1 << (sps->log2_min_luma_coding_block_size_minus3 + 3);
   unsigned ctb = min_cb << sps->log2_diff_max_min_luma_coding_block_size;
   unsigned log2_min_tb = sps->log2_min_transform_block_size_minus2 + 2;
   unsigned log2_max_tb = log2_min_tb + sps->log2_diff_max_min_transform_block_size;
   unsigned tile_cols = (unsigned)pps->num_tile_columns_minus1 + 1;
   unsigned tile_rows = (unsigned)pps->num_tile_rows_minus1 + 1;

   /* Progressive 8/10-bit 4:2:0 through direct Gallium NV12/P010 surfaces is
    * the validated stage. Range extensions, monochrome, separate colour
    * planes and 12-bit are rejected loudly; there is no other fallback.
    */
   if (picture->base.protected_playback || picture->IntraPicFlag > 1 ||
       sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag ||
       sps->bit_depth_luma_minus8 != sps->bit_depth_chroma_minus8 ||
       (bit_depth != 8 && bit_depth != 10) ||
       (bit_depth == 10) != (bpp == 2) ||
       sps->transform_skip_rotation_enabled_flag || sps->transform_skip_context_enabled_flag ||
       sps->implicit_rdpcm_enabled_flag || sps->explicit_rdpcm_enabled_flag ||
       sps->extended_precision_processing_flag || sps->intra_smoothing_disabled_flag ||
       sps->high_precision_offsets_enabled_flag || sps->persistent_rice_adaptation_enabled_flag ||
       sps->cabac_bypass_alignment_enabled_flag ||
       pps->cross_component_prediction_enabled_flag || pps->chroma_qp_offset_list_enabled_flag ||
       pps->log2_sao_offset_scale_luma || pps->log2_sao_offset_scale_chroma ||
       pps->log2_max_transform_skip_block_size_minus2 ||
       sps->log2_min_luma_coding_block_size_minus3 > 3 ||
       sps->log2_diff_max_min_luma_coding_block_size > 3 ||
       log2_min_tb < 2 || log2_min_tb > 5 || log2_max_tb > 5 || log2_max_tb > ctb ||
       sps->max_transform_hierarchy_depth_inter > 4 ||
       sps->max_transform_hierarchy_depth_intra > 4 ||
       sps->log2_max_pic_order_cnt_lsb_minus4 > 8 ||
       sps->num_short_term_ref_pic_sets > 64 || sps->num_long_term_ref_pics_sps > 32 ||
       pps->num_extra_slice_header_bits > 7 || pps->log2_parallel_merge_level_minus2 > 2 ||
       pps->num_ref_idx_l0_default_active_minus1 + 1u > 15 ||
       pps->num_ref_idx_l1_default_active_minus1 + 1u > 15 ||
       !tile_cols || tile_cols > NVDEC_HEVC_MAX_TILE_COLUMNS ||
       !tile_rows || tile_rows > NVDEC_HEVC_MAX_TILE_ROWS ||
       tile_cols * tile_rows > NVDEC_HEVC_MAX_TILE_COLUMNS * NVDEC_HEVC_MAX_TILE_ROWS ||
       !sps->pic_width_in_luma_samples || !sps->pic_height_in_luma_samples ||
       sps->pic_width_in_luma_samples % min_cb || sps->pic_height_in_luma_samples % min_cb ||
       sps->pic_width_in_luma_samples > storage_width ||
       sps->pic_height_in_luma_samples > storage_height ||
       storage_width > 4096 || storage_height > 4096 ||
       pitch_y % 64 || pitch_uv % 64 ||
       pitch_y / bpp > UINT16_MAX || pitch_uv / bpp > UINT16_MAX) {
      return false;
   }

   unsigned init_qp = 26 + pps->init_qp_minus26 + 6 * sps->bit_depth_luma_minus8;
   if (init_qp > 127)
      return false;

   memset(out, 0, sizeof(*out));
   nvdec_hevc_pic_s *p = &out->pic;

   unsigned skip;
   bool header_rps = false;
   if (!slice_skip_bits(picture, first_nal, nal_size, &skip, &header_rps))
      return false;

   p->stream_len = 0; /* the caller owns the assembled access unit size */
   p->tileformat = 1;
   p->gob_height = 0;
   p->sw_start_code_e = 1;
   p->framestride[0] = pitch_y / bpp;
   p->framestride[1] = pitch_uv / bpp;
   p->colMvBuffersize = nouveau_nvdec_hevc_coloc_size(storage_width, storage_height) >> 8;
   filter_layout(storage_width, storage_height, p);
   p->pic_width_in_luma_samples = sps->pic_width_in_luma_samples;
   p->pic_height_in_luma_samples = sps->pic_height_in_luma_samples;
   p->chroma_format_idc = sps->chroma_format_idc;
   p->bit_depth_luma = bit_depth;
   p->bit_depth_chroma = sps->bit_depth_chroma_minus8 + 8;
   p->log2_min_luma_coding_block_size = util_logbase2(min_cb);
   p->log2_max_luma_coding_block_size = util_logbase2(ctb);
   p->log2_min_transform_block_size = log2_min_tb;
   p->log2_max_transform_block_size = log2_max_tb;
   p->max_transform_hierarchy_depth_inter = sps->max_transform_hierarchy_depth_inter;
   p->max_transform_hierarchy_depth_intra = sps->max_transform_hierarchy_depth_intra;
   p->scalingListEnable = sps->scaling_list_enabled_flag;
   p->amp_enable_flag = sps->amp_enabled_flag;
   p->sample_adaptive_offset_enabled_flag = sps->sample_adaptive_offset_enabled_flag;
   p->pcm_enabled_flag = sps->pcm_enabled_flag;
   if (sps->pcm_enabled_flag) {
      p->pcm_sample_bit_depth_luma = sps->pcm_sample_bit_depth_luma_minus1 + 1;
      p->pcm_sample_bit_depth_chroma = sps->pcm_sample_bit_depth_chroma_minus1 + 1;
      p->log2_min_pcm_luma_coding_block_size =
         sps->log2_min_pcm_luma_coding_block_size_minus3 + 3;
      p->log2_max_pcm_luma_coding_block_size = p->log2_min_pcm_luma_coding_block_size +
         sps->log2_diff_max_min_pcm_luma_coding_block_size;
      p->pcm_loop_filter_disabled_flag = sps->pcm_loop_filter_disabled_flag;
   }
   p->sps_temporal_mvp_enabled_flag = sps->sps_temporal_mvp_enabled_flag;
   p->strong_intra_smoothing_enabled_flag = sps->strong_intra_smoothing_enabled_flag;
   p->dependent_slice_segments_enabled_flag = pps->dependent_slice_segments_enabled_flag;
   p->output_flag_present_flag = pps->output_flag_present_flag;
   p->num_extra_slice_header_bits = pps->num_extra_slice_header_bits;
   p->sign_data_hiding_enabled_flag = pps->sign_data_hiding_enabled_flag;
   p->cabac_init_present_flag = pps->cabac_init_present_flag;
   p->num_ref_idx_l0_default_active = pps->num_ref_idx_l0_default_active_minus1 + 1;
   p->num_ref_idx_l1_default_active = pps->num_ref_idx_l1_default_active_minus1 + 1;
   p->init_qp = init_qp;
   p->constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
   p->transform_skip_enabled_flag = pps->transform_skip_enabled_flag;
   if (pps->transform_skip_enabled_flag)
      p->v1.hevc_main10_444_ext.log2MaxTransformSkipSize = 2;
   p->cu_qp_delta_enabled_flag = pps->cu_qp_delta_enabled_flag;
   p->diff_cu_qp_delta_depth = pps->diff_cu_qp_delta_depth;
   p->pps_cb_qp_offset = pps->pps_cb_qp_offset;
   p->pps_cr_qp_offset = pps->pps_cr_qp_offset;
   p->pps_beta_offset = 2 * pps->pps_beta_offset_div2;
   p->pps_tc_offset = 2 * pps->pps_tc_offset_div2;
   p->pps_slice_chroma_qp_offsets_present_flag = pps->pps_slice_chroma_qp_offsets_present_flag;
   p->weighted_pred_flag = pps->weighted_pred_flag;
   p->weighted_bipred_flag = pps->weighted_bipred_flag;
   p->transquant_bypass_enabled_flag = pps->transquant_bypass_enabled_flag;
   p->tiles_enabled_flag = pps->tiles_enabled_flag;
   p->entropy_coding_sync_enabled_flag = pps->entropy_coding_sync_enabled_flag;
   p->num_tile_columns = tile_cols;
   p->num_tile_rows = tile_rows;
   p->loop_filter_across_tiles_enabled_flag = pps->loop_filter_across_tiles_enabled_flag;
   p->loop_filter_across_slices_enabled_flag = pps->pps_loop_filter_across_slices_enabled_flag;
   p->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
   p->deblocking_filter_override_enabled_flag = pps->deblocking_filter_override_enabled_flag;
   p->pps_deblocking_filter_disabled_flag = pps->pps_deblocking_filter_disabled_flag;
   p->lists_modification_present_flag = pps->lists_modification_present_flag;
   p->log2_parallel_merge_level = pps->log2_parallel_merge_level_minus2 + 2;
   p->slice_segment_header_extension_present_flag = pps->slice_segment_header_extension_present_flag;
   p->IDR_picture_flag = picture->IDRPicFlag;
   p->RAP_picture_flag = picture->RAPPicFlag;
   p->curr_pic_idx = current;
   p->sw_hdr_skip_length = skip;
   p->separate_colour_plane_flag = sps->separate_colour_plane_flag;
   p->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
   p->num_short_term_ref_pic_sets = sps->num_short_term_ref_pic_sets;
   p->num_long_term_ref_pics_sps = sps->num_long_term_ref_pics_sps;
   p->num_delta_pocs_of_rps_idx = picture->NumDeltaPocsOfRefRpsIdx;
   p->long_term_ref_pics_present_flag = sps->long_term_ref_pics_present_flag;
   p->num_bits_short_term_ref_pics_in_slice =
      header_rps ? picture->NumShortTermPictureSliceHeaderBits : 0;

   /* Tile sizes in CTBs per tile, raster order. */
   unsigned ctbs_x = DIV_ROUND_UP(sps->pic_width_in_luma_samples, ctb);
   unsigned ctbs_y = DIV_ROUND_UP(sps->pic_height_in_luma_samples, ctb);
   unsigned cols[NVDEC_HEVC_MAX_TILE_COLUMNS], rows[NVDEC_HEVC_MAX_TILE_ROWS];
   if (!pps->tiles_enabled_flag) {
      cols[0] = ctbs_x;
      rows[0] = ctbs_y;
   } else if (pps->uniform_spacing_flag) {
      for (unsigned i = 0; i < tile_cols; i++)
         cols[i] = (i + 1) * ctbs_x / tile_cols - i * ctbs_x / tile_cols;
      for (unsigned i = 0; i < tile_rows; i++)
         rows[i] = (i + 1) * ctbs_y / tile_rows - i * ctbs_y / tile_rows;
   } else {
      unsigned sum_x = 0, sum_y = 0;
      for (unsigned i = 0; i < tile_cols; i++) {
         cols[i] = pps->column_width_minus1[i] + 1;
         sum_x += cols[i];
      }
      for (unsigned i = 0; i < tile_rows; i++) {
         rows[i] = pps->row_height_minus1[i] + 1;
         sum_y += rows[i];
      }
      if (sum_x != ctbs_x || sum_y != ctbs_y)
         return false;
   }
   for (unsigned r = 0; r < tile_rows; r++)
      for (unsigned c = 0; c < tile_cols; c++) {
         if (!cols[c] || cols[c] > 255 || !rows[r] || rows[r] > 255)
            return false;
         out->tiles[2 * (r * tile_cols + c)] = cols[c];
         out->tiles[2 * (r * tile_cols + c) + 1] = rows[r];
      }

   /* Reference pictures: the sorted sets the frontend derived, as engine
    * slots. List 0: StCurrBefore, StCurrAfter, LtCurr; list 1 swaps the two
    * short-term sets. Missing entries must not happen: a picture that needs
    * a reference it never received is rejected, not guessed.
    */
   unsigned l0[16], l1[16], total = 0;
   for (unsigned pass = 0; pass < 3; pass++) {
      const uint8_t *set = pass == 0 ? picture->RefPicSetStCurrBefore :
                           pass == 1 ? picture->RefPicSetStCurrAfter :
                                       picture->RefPicSetLtCurr;
      unsigned count = pass == 0 ? picture->NumPocStCurrBefore :
                       pass == 1 ? picture->NumPocStCurrAfter : picture->NumPocLtCurr;
      if (count > 8)
         return false;
      for (unsigned i = 0; i < count; i++) {
         if (set[i] > 15 || references[set[i]] == 0xff)
            return false;
         l0[total] = references[set[i]];
         total++;
      }
   }
   /* List 1 starts with StCurrAfter; merge the remaining order. */
   unsigned n_after = 0;
   for (unsigned pass = 0; pass < 3; pass++) {
      const uint8_t *set = pass == 0 ? picture->RefPicSetStCurrAfter :
                           pass == 1 ? picture->RefPicSetStCurrBefore :
                                       picture->RefPicSetLtCurr;
      unsigned count = pass == 0 ? picture->NumPocStCurrAfter :
                       pass == 1 ? picture->NumPocStCurrBefore : picture->NumPocLtCurr;
      for (unsigned i = 0; i < count; i++)
         l1[n_after++] = references[set[i]];
   }
   p->num_ref_frames = total;
   for (unsigned i = 0; i < 16; i++) {
      p->initreflistidxl0[i] = total ? l0[i % total] : 0;
      p->initreflistidxl1[i] = total ? l1[i % total] : 0;
   }
   for (unsigned i = 0; i < 16; i++) {
      if (!picture->ref[i] || references[i] == 0xff)
         continue;
      int diff = picture->CurrPicOrderCntVal - picture->PicOrderCntVal[i];
      p->RefDiffPicOrderCnts[references[i]] = CLAMP(diff, -128, 127);
      if (picture->IsLongTerm[i])
         p->longtermflag |= 1u << (15 - references[i]);
   }

   write_scaling(out->scaling, sps);
   return true;
}
