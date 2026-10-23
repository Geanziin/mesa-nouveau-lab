/* SPDX-License-Identifier: MIT
 * NVDEC CDF placement follows the MIT implementation in mickys/nvenc,
 * src/av1probs.c (Copyright (c) 2026 mickys), derived there from Mesa's NVDEC
 * layout. Values are the standard AV1 defaults already shipped with Mesa.
 * Only constant tables are shared with AMD: no VCN commands/API are used.
 */
#include "nouveau_nvdec_av1.h"
#include "util/u_math.h"
#include "amd/common/ac_vcn_av1_default.h"
#include <assert.h>
#include <string.h>

static void
pack(void *table, unsigned offset, unsigned stride, const uint16_t *src,
     unsigned count, unsigned source_stride, unsigned values)
{
   assert(count && offset + ((count - 1) * stride + values) * 2 <= NVDEC_AV1_CDF_SIZE);
   for (unsigned i = 0; i < count; i++)
      memcpy((uint8_t *)table + offset + i * stride * 2,
             src + i * source_stride, values * sizeof(uint16_t));
}

#define PACK(off, stride, array, count, source_stride, values) \
   pack(table, off, stride, (const uint16_t *)(array), count, source_stride, values)

void
nouveau_nvdec_av1_default_cdf(unsigned qctx, void *table)
{
   assert(qctx < 4);
   memset(table, 0, NVDEC_AV1_CDF_SIZE);
   PACK(0x0000, 3, default_partition_cdf, 4, 11, 3);
   PACK(0x0018, 1, default_wiener_restore_cdf, 1, 3, 1);
   PACK(0x001a, 1, default_sgrproj_restore_cdf, 1, 3, 1);
   PACK(0x001c, 2, default_switchable_restore_cdf, 1, 4, 2);
   PACK(0x0020, 16, default_partition_cdf[4], 12, 11, 9);
   PACK(0x0132, 16, default_partition_cdf[16], 4, 11, 7);
   PACK(0x01a0, 12, default_kf_y_mode_cdf, 25, 14, 12);
   PACK(0x03f8, 1, default_segment_pred_cdf, 3, 3, 1);
   PACK(0x03fe, 7, default_spatial_pred_seg_tree_cdf, 3, 9, 7);
   PACK(0x0428, 1, default_skip_cdfs, 3, 3, 1);
   PACK(0x042e, 3, default_delta_q_cdf, 1, 5, 3);
   PACK(0x0434, 3, default_delta_lf_multi_cdf, 4, 5, 3);
   PACK(0x044c, 3, default_delta_lf_cdf, 1, 5, 3);
   PACK(0x0452, 1, default_skip_mode_cdfs, 3, 3, 1);
   PACK(0x0458, 1, default_txfm_partition_cdf, 21, 3, 1);
   PACK(0x0484, 2, default_tx_size_cdf[0], 3, 4, 1);
   PACK(0x0490, 2, default_tx_size_cdf[1], 9, 4, 2);
   PACK(0x04b4, 12, default_if_y_mode_cdf, 4, 14, 12);
   PACK(0x0514, 13, default_uv_mode_cdf[0], 13, 15, 12);
   PACK(0x0666, 13, default_uv_mode_cdf[1], 13, 15, 13);
   PACK(0x07b8, 1, default_intra_inter_cdf, 4, 3, 1);
   PACK(0x07c0, 1, default_comp_inter_cdf, 5, 3, 1);
   PACK(0x07ca, 1, default_single_ref_cdf, 18, 3, 1);
   PACK(0x07ee, 1, default_comp_ref_type_cdf, 5, 3, 1);
   PACK(0x07f8, 1, default_uni_comp_ref_cdf, 9, 3, 1);
   PACK(0x080a, 1, default_comp_ref_cdf, 9, 3, 1);
   PACK(0x081c, 1, default_comp_bwdref_cdf, 6, 3, 1);
   PACK(0x0828, 1, default_newmv_cdf, 6, 3, 1);
   PACK(0x0834, 1, default_zeromv_cdf, 2, 3, 1);
   PACK(0x0838, 1, default_refmv_cdf, 6, 3, 1);
   PACK(0x084a, 1, default_drl_cdf, 3, 3, 1);
   PACK(0x0850, 2, default_switchable_interp_cdf, 16, 4, 2);
   PACK(0x0890, 3, default_nmv_context.joints_cdf, 1, 5, 3);
   for (unsigned c = 0; c < 2; c++) {
      const av1_nmv_component *mv = &default_nmv_context.comps[c];
      PACK(0x0896 + c * 2, 1, mv->sign_cdf, 1, 3, 1);
      PACK(0x089a + c * 20, 10, mv->classes_cdf, 1, 12, 10);
      PACK(0x08c2 + c * 12, 3, mv->class0_fp_cdf, 2, 5, 3);
      PACK(0x08da + c * 6, 3, mv->fp_cdf, 1, 5, 3);
      PACK(0x08e6 + c * 2, 1, mv->class0_hp_cdf, 1, 3, 1);
      PACK(0x08ea + c * 2, 1, mv->hp_cdf, 1, 3, 1);
      PACK(0x08ee + c * 2, 1, mv->class0_cdf, 1, 3, 1);
      PACK(0x08f2 + c * 20, 1, mv->bits_cdf, 10, 3, 1);
   }
   PACK(0x091a, 1, default_obmc_cdf, 22, 3, 1);
   PACK(0x0946, 2, default_motion_mode_cdf, 22, 4, 2);
   PACK(0x099e, 7, default_inter_compound_mode_cdf, 8, 9, 7);
   PACK(0x0a70, 1, default_compound_type_cdf, 22, 3, 1);
   PACK(0x0a9c, 1, default_interintra_cdf, 4, 3, 1);
   PACK(0x0aa4, 3, default_interintra_mode_cdf, 4, 5, 3);
   PACK(0x0abc, 1, default_wedge_interintra_cdf, 22, 3, 1);
   PACK(0x0ae8, 15, default_wedge_idx_cdf, 22, 17, 15);
   PACK(0x0d7c, 1, default_palette_y_mode_cdf, 21, 3, 1);
   PACK(0x0da6, 1, default_palette_uv_mode_cdf, 2, 3, 1);
   PACK(0x0daa, 6, default_palette_y_size_cdf, 7, 8, 6);
   PACK(0x0dfe, 6, default_palette_uv_size_cdf, 7, 8, 6);
   PACK(0x0e52, 7, default_cfl_sign_cdf, 1, 9, 7);
   PACK(0x0e60, 15, default_cfl_alpha_cdf, 6, 17, 15);
   PACK(0x0f14, 1, default_intrabc_cdf, 1, 3, 1);
   PACK(0x0f16, 6, default_angle_delta_cdf, 8, 8, 6);
   PACK(0x0f76, 4, default_filter_intra_mode_cdf, 1, 6, 4);
   PACK(0x0f7e, 1, default_filter_intra_cdfs, 22, 3, 1);
   PACK(0x0faa, 1, default_comp_group_idx_cdfs, 6, 3, 1);
   PACK(0x0fb8, 1, default_compound_idx_cdfs, 6, 3, 1);
   const unsigned offsets[] = {0x00, 0x50, 0xa0, 0xf0, 0xa6, 0x54, 0x02};
   for (unsigned uv = 0; uv < 2; uv++) {
      for (unsigned n = 2; n <= 8; n++) {
         const uint16_t *src = uv ? &default_palette_uv_color_index_cdf[n - 2][0][0] :
                                    &default_palette_y_color_index_cdf[n - 2][0][0];
         PACK((uv ? 0x1100 : 0x0fe0) + offsets[n - 2], n == 5 ? 4 : 8, src, 5, 9, n - 1);
      }
   }
   PACK(0x1220, 8, default_intra_ext_tx_cdf[1], 4 * 13, 17, 6);
   PACK(0x1560, 4, default_intra_ext_tx_cdf[2], 4 * 13, 17, 4);
   PACK(0x1700, 16, default_inter_ext_tx_cdf[1], 4, 17, 15);
   PACK(0x1780, 16, default_inter_ext_tx_cdf[2], 4, 17, 11);
   PACK(0x1796, 16, default_inter_ext_tx_cdf[3], 4, 17, 1);
   PACK(0x1800, 1, av1_default_txb_skip_cdfs[qctx], 5 * 13, 3, 1);
   for (unsigned t = 0; t < 5; t++)
      for (unsigned p = 0; p < 2; p++)
         PACK(0x1882 + (t * 2 + p) * 18, 1, av1_default_eob_extra_cdfs[qctx][t][p][3], 9, 3, 1);
   PACK(0x1940, 4, av1_default_eob_multi16_cdfs[qctx], 4, 6, 4);
   PACK(0x1960, 8, av1_default_eob_multi32_cdfs[qctx], 4, 7, 5);
   PACK(0x19a0, 8, av1_default_eob_multi64_cdfs[qctx], 4, 8, 6);
   PACK(0x19e0, 8, av1_default_eob_multi128_cdfs[qctx], 4, 9, 7);
   PACK(0x1a20, 8, av1_default_eob_multi256_cdfs[qctx], 4, 10, 8);
   for (unsigned p = 0; p < 2; p++) {
      PACK(0x1a60 + p * 64, 16, av1_default_eob_multi512_cdfs[qctx][p][0], 1, 11, 9);
      PACK(0x1ae0 + p * 64, 16, av1_default_eob_multi1024_cdfs[qctx][p][0], 1, 12, 10);
      /* The unused second coefficient context is uniform in the NVDEC ABI. */
      uint16_t uniform[10];
      for (unsigned i = 0; i < 10; i++)
         uniform[i] = 32768 - (32768 * (i + 1) + 5) / 10;
      PACK(0x1a80 + p * 64, 16, uniform, 1, 10, 9);
      for (unsigned i = 0; i < 10; i++)
         uniform[i] = 32768 - (32768 * (i + 1) + 5) / 11;
      PACK(0x1b00 + p * 64, 16, uniform, 1, 10, 10);
   }
   PACK(0x1b60, 2, av1_default_coeff_base_eob_multi_cdfs[qctx], 5 * 2 * 4, 4, 2);
   PACK(0x1c00, 4, av1_default_coeff_base_multi_cdfs[qctx], 5 * 2 * 42, 5, 3);
   PACK(0x2920, 1, av1_default_dc_sign_cdfs[qctx], 2 * 3, 3, 1);
   PACK(0x2930, 4, av1_default_coeff_lps_multi_cdfs[qctx], 5 * 2 * 21, 5, 3);
}
