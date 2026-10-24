/* SPDX-License-Identifier: MIT */
#ifndef NOUVEAU_NVDEC_VP9_H
#define NOUVEAU_NVDEC_VP9_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
   VP9_KEY_FRAME = 0,
   VP9_NON_KEY_FRAME = 1,
   VP9_EIGHTTAP = 0,
   VP9_EIGHTTAP_SMOOTH = 1,
   VP9_EIGHTTAP_SHARP = 2,
   VP9_BILINEAR = 3,
   VP9_SWITCHABLE = 4,
   VP9_ONLY_4X4 = 0,
   VP9_ALLOW_8X8 = 1,
   VP9_ALLOW_16X16 = 2,
   VP9_ALLOW_32X32 = 3,
   VP9_TX_MODE_SELECT = 4,
   VP9_SINGLE_REFERENCE = 0,
   VP9_COMPOUND_REFERENCE = 1,
   VP9_REFERENCE_MODE_SELECT = 2,
   VP9_INTRA_FRAME = 0,
   VP9_LAST_FRAME = 1,
   VP9_GOLDEN_FRAME = 2,
   VP9_ALTREF_FRAME = 3,
   VP9_CS_RGB = 7,
};

/* Every adaptive probability table, in the spec's shapes and names.
 * Layout reference: mickys/nvenc, src/vp9parse.h (MIT, Copyright 2026 mickys). */
struct nouveau_vp9_probs {
   uint8_t tx8[2][1], tx16[2][2], tx32[2][3];
   uint8_t coef[4][2][2][6][6][3];
   uint8_t skip[3];
   uint8_t inter_mode[7][3];
   uint8_t interp_filter[4][2];
   uint8_t is_inter[4];
   uint8_t comp_mode[5];
   uint8_t single_ref[5][2];
   uint8_t comp_ref[5];
   uint8_t y_mode[4][9];
   uint8_t uv_mode[10][9];
   uint8_t partition[16][3];
   uint8_t mv_joint[3];
   uint8_t mv_sign[2];
   uint8_t mv_class[2][10];
   uint8_t mv_class0_bit[2];
   uint8_t mv_bits[2][10];
   uint8_t mv_class0_fr[2][2][3];
   uint8_t mv_fr[2][3];
   uint8_t mv_class0_hp[2];
   uint8_t mv_hp[2];
};

/* One frame's headers (uncompressed + compressed). */
struct nouveau_vp9_frame {
   int profile, show_existing_frame, frame_to_show_map_idx;
   int frame_type, show_frame, error_resilient_mode, intra_only, frame_is_intra;
   int reset_frame_context;
   int bit_depth, color_space, color_range, subsampling_x, subsampling_y;
   int refresh_frame_flags, ref_frame_idx[3], ref_frame_sign_bias[4];
   int width, height;
   int size_from_ref, size_ref_idx; /* the frame takes this reference's size */
   int allow_high_precision_mv, interp_filter;
   int refresh_frame_context, frame_parallel_decoding_mode, frame_context_idx;
   int lf_level, lf_sharpness;
   int base_q_idx, delta_q_y_dc, delta_q_uv_dc, delta_q_uv_ac, lossless;
   int seg_enabled, seg_update_map, seg_temporal_update, seg_update_data;
   uint8_t seg_tree_probs[7], seg_pred_probs[3];
   int tile_cols_log2, tile_rows_log2;
   size_t uncomp_bytes, header_size_in_bytes; /* the tile data starts after both */
   int tx_mode, reference_mode, comp_fixed_ref, comp_var_ref[2];
   int last_frame_type;
};

/* What persists from frame to frame. */
struct nouveau_vp9_state {
   struct nouveau_vp9_probs ctx[4]; /* the saved frame contexts */
   struct nouveau_vp9_probs fc;    /* this frame's probabilities */
   int frame_type;                 /* of the last parsed frame */
   int bit_depth, subsampling_x, subsampling_y, profile;
   int lf_delta_enabled, lf_ref_deltas[4], lf_mode_deltas[2];
   int seg_abs_or_delta_update, seg_feature_enabled[8][4], seg_feature_data[8][4];
   int interp_filter, allow_high_precision_mv;
   bool started; /* a key frame was seen */
};

void nouveau_vp9_init(struct nouveau_vp9_state *state);
/* Parse one frame's headers; updates the state (contexts reset/loaded,
 * probabilities updated from the compressed header). VP9 spec 6 and 8.4. */
bool nouveau_vp9_parse(struct nouveau_vp9_state *state, const uint8_t *data,
                       size_t size, unsigned ref_width, unsigned ref_height,
                       struct nouveau_vp9_frame *frame);
/* Backward adaptation from the engine's counter buffer (an
 * nvdec_vp9EntropyCounts_t; NULL when the frame does not adapt) and the
 * frame-context save. VP9 spec 8.4 and 6.1.2. */
void nouveau_vp9_end_frame(struct nouveau_vp9_state *state,
                           const struct nouveau_vp9_frame *frame,
                           const void *counts);
static inline bool
nouveau_vp9_adapts(const struct nouveau_vp9_frame *frame)
{
   return !frame->error_resilient_mode && !frame->frame_parallel_decoding_mode;
}
/* Pack the frame's probabilities as the NVDEC engine reads them. */
void nouveau_vp9_pack_probs(void *table, const struct nouveau_vp9_state *state,
                            const struct nouveau_vp9_frame *frame);

unsigned nouveau_vp9_filter_size(unsigned width, unsigned height);
unsigned nouveau_vp9_seg_size(unsigned width, unsigned height);
unsigned nouveau_vp9_colmv_size(unsigned width, unsigned height);

#endif
