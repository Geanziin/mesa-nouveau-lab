/* SPDX-License-Identifier: MIT
 * Native Gallium VP9 state: headers, probability contexts and backward
 * adaptation, following the VP9 Bitstream & Decoding Process Specification
 * v0.7 (sections 6.1-6.3, 7.2-7.3, 8.4, 9.2). The NVDEC probability-table
 * layout and the engine counter semantics follow mickys/nvenc,
 * src/vp9parse.c (MIT, Copyright 2026 mickys), which this port keeps close
 * to for reviewability. No Vulkan objects or APIs are involved anywhere.
 */
#include "nouveau_nvdec_vp9.h"
#include "nouveau_nvdec_vp9_tables.h"
#include "nvdec_drv.h"
#include "util/u_math.h"
#include <string.h>

#define NVDEC_ALIGN256(x) align64(x, 256)

/* ---- bit reader (f(n), s(n)) ---- */
struct vp9_bits {
   const uint8_t *data;
   size_t size, bit;
   bool err;
};

static unsigned
read_1(struct vp9_bits *r)
{
   if (r->bit >= r->size * 8) {
      r->err = true;
      return 0;
   }
   unsigned v = r->data[r->bit >> 3] >> (7 - (r->bit & 7)) & 1;
   r->bit++;
   return v;
}

static unsigned
read_n(struct vp9_bits *r, int n)
{
   unsigned v = 0;
   while (n-- > 0)
      v = v << 1 | read_1(r);
   return v;
}

static int
read_signed(struct vp9_bits *r, int n)
{
   int v = (int)read_n(r, n);
   return read_1(r) ? -v : v;
}

/* ---- boolean decoder (9.2) ---- */
struct vp9_bool {
   struct vp9_bits bits;
   unsigned value, range;
   long maxbits;
};

static bool
bool_init(struct vp9_bool *d, const uint8_t *data, size_t size)
{
   if (size < 1)
      return false;
   d->bits = (struct vp9_bits){ data, size, 0, false };
   d->value = read_n(&d->bits, 8);
   d->range = 255;
   d->maxbits = 8 * (long)size - 8;
   return true;
}

static int
bool_read(struct vp9_bool *d, int prob)
{
   const unsigned split = 1 + (((d->range - 1) * (unsigned)prob) >> 8);
   int bit;
   if (d->value < split) {
      d->range = split;
      bit = 0;
   } else {
      d->range -= split;
      d->value -= split;
      bit = 1;
   }
   while (d->range < 128) {
      unsigned next = 0;
      if (d->maxbits > 0) {
         next = read_1(&d->bits);
         d->maxbits--;
      }
      d->range <<= 1;
      d->value = (d->value << 1) + next;
   }
   return bit;
}

static unsigned
bool_literal(struct vp9_bool *d, int n)
{
   unsigned x = 0;
   while (n-- > 0)
      x = 2 * x + (unsigned)bool_read(d, 128);
   return x;
}

/* ---- defaults and contexts ---- */
static void
default_probs(struct nouveau_vp9_probs *p)
{
   for (int i = 0; i < 2; i++) {
      p->tx8[i][0] = default_tx_probs[1][i][0];
      for (int j = 0; j < 2; j++)
         p->tx16[i][j] = default_tx_probs[2][i][j];
      for (int j = 0; j < 3; j++)
         p->tx32[i][j] = default_tx_probs[3][i][j];
   }
   memcpy(p->coef, default_coef_probs, sizeof(p->coef));
   memcpy(p->skip, default_skip_prob, sizeof(p->skip));
   memcpy(p->inter_mode, default_inter_mode_probs, sizeof(p->inter_mode));
   memcpy(p->interp_filter, default_interp_filter_probs, sizeof(p->interp_filter));
   memcpy(p->is_inter, default_is_inter_prob, sizeof(p->is_inter));
   memcpy(p->comp_mode, default_comp_mode_prob, sizeof(p->comp_mode));
   memcpy(p->single_ref, default_single_ref_prob, sizeof(p->single_ref));
   memcpy(p->comp_ref, default_comp_ref_prob, sizeof(p->comp_ref));
   memcpy(p->y_mode, default_y_mode_probs, sizeof(p->y_mode));
   memcpy(p->uv_mode, default_uv_mode_probs, sizeof(p->uv_mode));
   memcpy(p->partition, default_partition_probs, sizeof(p->partition));
   memcpy(p->mv_joint, default_mv_joint_probs, sizeof(p->mv_joint));
   memcpy(p->mv_sign, default_mv_sign_prob, sizeof(p->mv_sign));
   memcpy(p->mv_class, default_mv_class_probs, sizeof(p->mv_class));
   memcpy(p->mv_class0_bit, default_mv_class0_bit_prob, sizeof(p->mv_class0_bit));
   memcpy(p->mv_bits, default_mv_bits_prob, sizeof(p->mv_bits));
   memcpy(p->mv_class0_fr, default_mv_class0_fr_probs, sizeof(p->mv_class0_fr));
   memcpy(p->mv_fr, default_mv_fr_probs, sizeof(p->mv_fr));
   memcpy(p->mv_class0_hp, default_mv_class0_hp_prob, sizeof(p->mv_class0_hp));
   memcpy(p->mv_hp, default_mv_hp_prob, sizeof(p->mv_hp));
}

void
nouveau_vp9_init(struct nouveau_vp9_state *state)
{
   memset(state, 0, sizeof(*state));
   default_probs(&state->fc);
   for (int i = 0; i < 4; i++)
      state->ctx[i] = state->fc;
   state->frame_type = VP9_KEY_FRAME;
}

static void
setup_past_independence(struct nouveau_vp9_state *state)
{
   memset(state->seg_feature_enabled, 0, sizeof(state->seg_feature_enabled));
   memset(state->seg_feature_data, 0, sizeof(state->seg_feature_data));
   state->seg_abs_or_delta_update = 0;
   state->lf_delta_enabled = 1;
   state->lf_ref_deltas[0] = 1;
   state->lf_ref_deltas[1] = 0;
   state->lf_ref_deltas[2] = -1;
   state->lf_ref_deltas[3] = -1;
   state->lf_mode_deltas[0] = state->lf_mode_deltas[1] = 0;
   default_probs(&state->fc);
}

/* ---- uncompressed header (6.2) ---- */
static void
color_config(struct vp9_bits *r, struct nouveau_vp9_state *state,
             struct nouveau_vp9_frame *h)
{
   h->bit_depth = h->profile >= 2 ? (read_1(r) ? 12 : 10) : 8;
   h->color_space = (int)read_n(r, 3);
   if (h->color_space != VP9_CS_RGB) {
      h->color_range = (int)read_1(r);
      if (h->profile == 1 || h->profile == 3) {
         h->subsampling_x = (int)read_1(r);
         h->subsampling_y = (int)read_1(r);
         read_1(r);
      } else {
         h->subsampling_x = h->subsampling_y = 1;
      }
   } else {
      h->color_range = 1;
      if (h->profile == 1 || h->profile == 3) {
         h->subsampling_x = h->subsampling_y = 0;
         read_1(r);
      }
   }
   state->bit_depth = h->bit_depth;
   state->subsampling_x = h->subsampling_x;
   state->subsampling_y = h->subsampling_y;
}

static void
frame_size(struct vp9_bits *r, struct nouveau_vp9_frame *h)
{
   h->width = (int)read_n(r, 16) + 1;
   h->height = (int)read_n(r, 16) + 1;
}

static void
render_size(struct vp9_bits *r, struct nouveau_vp9_frame *h)
{
   if (read_1(r)) { /* render_and_frame_size_different */
      read_n(r, 16);
      read_n(r, 16);
   }
}

static bool
sync_code(struct vp9_bits *r)
{
   return read_n(r, 8) == 0x49 && read_n(r, 8) == 0x83 && read_n(r, 8) == 0x42;
}

static uint8_t
read_prob(struct vp9_bits *r)
{
   return read_1(r) ? (uint8_t)read_n(r, 8) : 255;
}

static int
read_delta_q(struct vp9_bits *r)
{
   return read_1(r) ? read_signed(r, 4) : 0;
}

/* ---- compressed header (6.3) ---- */
static const uint8_t inv_map_table[255] = {
    7, 20, 33, 46, 59, 72, 85, 98, 111, 124, 137, 150, 163, 176, 189, 202, 215, 228, 241, 254, 1, 2, 3, 4, 5, 6, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 99, 100, 101, 102,
    103, 104, 105, 106, 107, 108, 109, 110, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 151, 152,
    153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 177,
    178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201,
    203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226,
    227, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251,
    252, 253, 253,
};

static int
inv_recenter_nonneg(int v, int m)
{
   if (v > 2 * m)
      return v;
   return (v & 1) ? m - ((v + 1) >> 1) : m + (v >> 1);
}

static int
decode_term_subexp(struct vp9_bool *d)
{
   if (!bool_literal(d, 1))
      return (int)bool_literal(d, 4);
   if (!bool_literal(d, 1))
      return (int)bool_literal(d, 4) + 16;
   if (!bool_literal(d, 1))
      return (int)bool_literal(d, 5) + 32;
   const int v = (int)bool_literal(d, 7);
   if (v < 65)
      return v + 64;
   return (v << 1) - 1 + (int)bool_literal(d, 1);
}

static int
inv_remap_prob(int delta, int prob)
{
   const int v = inv_map_table[delta < 255 ? delta : 254];
   int m = prob - 1;
   if ((m << 1) <= 255)
      return 1 + inv_recenter_nonneg(v, m);
   return 255 - inv_recenter_nonneg(v, 255 - 1 - m);
}

static void
diff_update(struct vp9_bool *d, uint8_t *p)
{
   if (bool_read(d, 252))
      *p = (uint8_t)inv_remap_prob(decode_term_subexp(d), *p);
}

static void
mv_update(struct vp9_bool *d, uint8_t *p)
{
   if (bool_read(d, 252))
      *p = (uint8_t)((bool_literal(d, 7) << 1) | 1);
}

static bool
compressed_header(struct vp9_bool *d, struct nouveau_vp9_state *state,
                  struct nouveau_vp9_frame *h)
{
   struct nouveau_vp9_probs *p = &state->fc;
   /* tx mode */
   if (h->lossless) {
      h->tx_mode = VP9_ONLY_4X4;
   } else {
      h->tx_mode = (int)bool_literal(d, 2);
      if (h->tx_mode == VP9_ALLOW_32X32)
         h->tx_mode += (int)bool_literal(d, 1);
   }
   if (h->tx_mode == VP9_TX_MODE_SELECT) {
      for (int i = 0; i < 2; i++)
         diff_update(d, &p->tx8[i][0]);
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 2; j++)
            diff_update(d, &p->tx16[i][j]);
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 3; j++)
            diff_update(d, &p->tx32[i][j]);
   }
   /* coefficient probabilities */
   static const int biggest_tx[5] = { 0, 1, 2, 3, 3 };
   for (int t = 0; t <= biggest_tx[h->tx_mode]; t++) {
      if (!bool_literal(d, 1))
         continue;
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 2; j++)
            for (int k = 0; k < 6; k++)
               for (int l = 0; l < (k == 0 ? 3 : 6); l++)
                  for (int m = 0; m < 3; m++)
                     diff_update(d, &p->coef[t][i][j][k][l][m]);
   }
   for (int i = 0; i < 3; i++)
      diff_update(d, &p->skip[i]);
   if (h->frame_is_intra)
      return !d->bits.err;
   for (int i = 0; i < 7; i++)
      for (int j = 0; j < 3; j++)
         diff_update(d, &p->inter_mode[i][j]);
   if (h->interp_filter == VP9_SWITCHABLE)
      for (int j = 0; j < 4; j++)
         for (int i = 0; i < 2; i++)
            diff_update(d, &p->interp_filter[j][i]);
   for (int i = 0; i < 4; i++)
      diff_update(d, &p->is_inter[i]);
   /* frame reference mode */
   int compound_allowed = 0;
   for (int i = 1; i < 3; i++)
      if (h->ref_frame_sign_bias[i + 1] != h->ref_frame_sign_bias[1])
         compound_allowed = 1;
   h->reference_mode = VP9_SINGLE_REFERENCE;
   if (compound_allowed && bool_literal(d, 1)) {
      h->reference_mode = bool_literal(d, 1) ? VP9_REFERENCE_MODE_SELECT
                                             : VP9_COMPOUND_REFERENCE;
      const int *bias = h->ref_frame_sign_bias;
      if (bias[VP9_LAST_FRAME] == bias[VP9_GOLDEN_FRAME]) {
         h->comp_fixed_ref = VP9_ALTREF_FRAME;
         h->comp_var_ref[0] = VP9_LAST_FRAME;
         h->comp_var_ref[1] = VP9_GOLDEN_FRAME;
      } else if (bias[VP9_LAST_FRAME] == bias[VP9_ALTREF_FRAME]) {
         h->comp_fixed_ref = VP9_GOLDEN_FRAME;
         h->comp_var_ref[0] = VP9_LAST_FRAME;
         h->comp_var_ref[1] = VP9_ALTREF_FRAME;
      } else {
         h->comp_fixed_ref = VP9_LAST_FRAME;
         h->comp_var_ref[0] = VP9_GOLDEN_FRAME;
         h->comp_var_ref[1] = VP9_ALTREF_FRAME;
      }
   }
   if (h->reference_mode == VP9_REFERENCE_MODE_SELECT)
      for (int i = 0; i < 5; i++)
         diff_update(d, &p->comp_mode[i]);
   if (h->reference_mode != VP9_COMPOUND_REFERENCE)
      for (int i = 0; i < 5; i++) {
         diff_update(d, &p->single_ref[i][0]);
         diff_update(d, &p->single_ref[i][1]);
      }
   if (h->reference_mode != VP9_SINGLE_REFERENCE)
      for (int i = 0; i < 5; i++)
         diff_update(d, &p->comp_ref[i]);
   for (int i = 0; i < 4; i++)
      for (int j = 0; j < 9; j++)
         diff_update(d, &p->y_mode[i][j]);
   for (int i = 0; i < 16; i++)
      for (int j = 0; j < 3; j++)
         diff_update(d, &p->partition[i][j]);
   /* motion vector probabilities */
   for (int j = 0; j < 3; j++)
      mv_update(d, &p->mv_joint[j]);
   for (int i = 0; i < 2; i++) {
      mv_update(d, &p->mv_sign[i]);
      for (int j = 0; j < 10; j++)
         mv_update(d, &p->mv_class[i][j]);
      mv_update(d, &p->mv_class0_bit[i]);
      for (int j = 0; j < 10; j++)
         mv_update(d, &p->mv_bits[i][j]);
   }
   for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 2; j++)
         for (int k = 0; k < 3; k++)
            mv_update(d, &p->mv_class0_fr[i][j][k]);
      for (int k = 0; k < 3; k++)
         mv_update(d, &p->mv_fr[i][k]);
   }
   if (h->allow_high_precision_mv)
      for (int i = 0; i < 2; i++) {
         mv_update(d, &p->mv_class0_hp[i]);
         mv_update(d, &p->mv_hp[i]);
      }
   return !d->bits.err;
}

bool
nouveau_vp9_parse(struct nouveau_vp9_state *state, const uint8_t *data,
                  size_t size, unsigned ref_width, unsigned ref_height,
                  struct nouveau_vp9_frame *frame)
{
   struct vp9_bits r = { data, size, 0, false };
   memset(frame, 0, sizeof(*frame));
   if (read_n(&r, 2) != 2)
      return false;
   frame->profile = (int)read_1(&r);
   frame->profile |= (int)read_1(&r) << 1;
   if (frame->profile == 3)
      read_1(&r);
   frame->show_existing_frame = (int)read_1(&r);
   if (frame->show_existing_frame) {
      frame->frame_to_show_map_idx = (int)read_n(&r, 3);
      return !r.err;
   }
   frame->last_frame_type = state->frame_type;
   frame->frame_type = (int)read_1(&r);
   frame->show_frame = (int)read_1(&r);
   frame->error_resilient_mode = (int)read_1(&r);
   frame->bit_depth = state->bit_depth;
   frame->subsampling_x = state->subsampling_x;
   frame->subsampling_y = state->subsampling_y;
   if (frame->frame_type == VP9_KEY_FRAME) {
      if (!sync_code(&r))
         return false;
      color_config(&r, state, frame);
      frame_size(&r, frame);
      render_size(&r, frame);
      frame->refresh_frame_flags = 0xff;
      frame->frame_is_intra = 1;
   } else {
      frame->intra_only = frame->show_frame ? 0 : (int)read_1(&r);
      if (!state->started && !frame->intra_only)
         return false; /* a stream starts with a key frame (8.2) */
      frame->frame_is_intra = frame->intra_only;
      frame->reset_frame_context = frame->error_resilient_mode ? 0 : (int)read_n(&r, 2);
      if (frame->intra_only) {
         if (!sync_code(&r))
            return false;
         if (frame->profile > 0) {
            color_config(&r, state, frame);
         } else {
            frame->subsampling_x = frame->subsampling_y = 1;
            frame->bit_depth = 8;
            state->subsampling_x = state->subsampling_y = 1;
            state->bit_depth = 8;
         }
         frame->refresh_frame_flags = (int)read_n(&r, 8);
         frame_size(&r, frame);
         render_size(&r, frame);
      } else {
         frame->refresh_frame_flags = (int)read_n(&r, 8);
         for (int i = 0; i < 3; i++) {
            frame->ref_frame_idx[i] = (int)read_n(&r, 3);
            frame->ref_frame_sign_bias[VP9_LAST_FRAME + i] = (int)read_1(&r);
         }
         /* ref_frame_sign_bias[] in the spec is indexed from INTRA (0). */
         for (int i = 0; i < 3; i++) {
            if (read_1(&r)) {
               /* The frame takes this reference's size; the tile syntax
                * depends on it, so resolve it here (7.2.4). All marked
                * references have the same size, which the frontend reports.
                */
               frame->size_from_ref = 1;
               frame->size_ref_idx = i;
               frame->width = ref_width;
               frame->height = ref_height;
               break;
            }
         }
         if (!frame->size_from_ref)
            frame_size(&r, frame);
         render_size(&r, frame);
         frame->allow_high_precision_mv = (int)read_1(&r);
         if (read_1(&r)) {
            frame->interp_filter = VP9_SWITCHABLE;
         } else {
            static const int literal_to_type[4] = {
               VP9_EIGHTTAP_SMOOTH, VP9_EIGHTTAP, VP9_EIGHTTAP_SHARP, VP9_BILINEAR,
            };
            frame->interp_filter = literal_to_type[read_n(&r, 2)];
         }
      }
   }
   if (!frame->error_resilient_mode) {
      frame->refresh_frame_context = (int)read_1(&r);
      frame->frame_parallel_decoding_mode = (int)read_1(&r);
   } else {
      frame->refresh_frame_context = 0;
      frame->frame_parallel_decoding_mode = 1;
   }
   frame->frame_context_idx = (int)read_n(&r, 2);
   if (frame->frame_is_intra || frame->error_resilient_mode) {
      setup_past_independence(state);
      if (frame->frame_type == VP9_KEY_FRAME || frame->error_resilient_mode ||
          frame->reset_frame_context == 3) {
         for (int i = 0; i < 4; i++)
            state->ctx[i] = state->fc;
      } else if (frame->reset_frame_context == 2) {
         state->ctx[frame->frame_context_idx] = state->fc;
      }
      frame->frame_context_idx = 0;
      memset(frame->ref_frame_sign_bias, 0, sizeof(frame->ref_frame_sign_bias));
   }
   /* loop filter */
   frame->lf_level = (int)read_n(&r, 6);
   frame->lf_sharpness = (int)read_n(&r, 3);
   state->lf_delta_enabled = (int)read_1(&r);
   if (state->lf_delta_enabled && read_1(&r)) {
      for (int i = 0; i < 4; i++)
         if (read_1(&r))
            state->lf_ref_deltas[i] = read_signed(&r, 6);
      for (int i = 0; i < 2; i++)
         if (read_1(&r))
            state->lf_mode_deltas[i] = read_signed(&r, 6);
   }
   /* quantization */
   frame->base_q_idx = (int)read_n(&r, 8);
   frame->delta_q_y_dc = read_delta_q(&r);
   frame->delta_q_uv_dc = read_delta_q(&r);
   frame->delta_q_uv_ac = read_delta_q(&r);
   frame->lossless = frame->base_q_idx == 0 && frame->delta_q_y_dc == 0 &&
                     frame->delta_q_uv_dc == 0 && frame->delta_q_uv_ac == 0;
   /* segmentation */
   frame->seg_enabled = (int)read_1(&r);
   if (frame->seg_enabled) {
      frame->seg_update_map = (int)read_1(&r);
      if (frame->seg_update_map) {
         for (int i = 0; i < 7; i++)
            frame->seg_tree_probs[i] = read_prob(&r);
         frame->seg_temporal_update = (int)read_1(&r);
         for (int i = 0; i < 3; i++)
            frame->seg_pred_probs[i] = frame->seg_temporal_update ? read_prob(&r) : 255;
      }
      frame->seg_update_data = (int)read_1(&r);
      if (frame->seg_update_data) {
         static const int bits[4] = { 8, 6, 2, 0 }, sgn[4] = { 1, 1, 0, 0 };
         state->seg_abs_or_delta_update = (int)read_1(&r);
         for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) {
               int v = 0;
               state->seg_feature_enabled[i][j] = (int)read_1(&r);
               if (state->seg_feature_enabled[i][j]) {
                  v = (int)read_n(&r, bits[j]);
                  if (sgn[j] && read_1(&r))
                     v = -v;
               }
               state->seg_feature_data[i][j] = v;
            }
      }
   }
   /* tiles */
   const int mi_cols = (frame->width + 7) >> 3, sb64_cols = (mi_cols + 7) >> 3;
   int min_log2 = 0;
   while ((64 << min_log2) < sb64_cols)
      min_log2++;
   int max_log2 = 1;
   while ((sb64_cols >> max_log2) >= 4)
      max_log2++;
   max_log2--;
   frame->tile_cols_log2 = min_log2;
   while (frame->tile_cols_log2 < max_log2 && read_1(&r))
      frame->tile_cols_log2++;
   frame->tile_rows_log2 = (int)read_1(&r);
   if (frame->tile_rows_log2)
      frame->tile_rows_log2 += (int)read_1(&r);
   frame->header_size_in_bytes = read_n(&r, 16);
   frame->uncomp_bytes = (r.bit + 7) >> 3;
   if (r.err || frame->header_size_in_bytes == 0 ||
       frame->uncomp_bytes + frame->header_size_in_bytes > size)
      return false;
   state->frame_type = frame->frame_type;
   state->profile = frame->profile;
   state->interp_filter = frame->interp_filter;
   state->allow_high_precision_mv = frame->allow_high_precision_mv;
   if (frame->frame_is_intra)
      state->started = true;
   /* load_probs, then the compressed header's updates */
   state->fc = state->ctx[frame->frame_context_idx];
   struct vp9_bool decoder;
   if (!bool_init(&decoder, data + frame->uncomp_bytes, frame->header_size_in_bytes) ||
       bool_read(&decoder, 128))
      return false;
   return compressed_header(&decoder, state, frame);
}

/* ---- backward adaptation (8.4) ---- */
static uint8_t
merge_prob(int pre, unsigned ct0, unsigned ct1, unsigned sat, unsigned maxf)
{
   const unsigned den = ct0 + ct1;
   unsigned prob = 128;
   if (den) {
      uint64_t q = ((uint64_t)ct0 * 256 + (den >> 1)) / den;
      prob = q < 1 ? 1 : q > 255 ? 255 : (unsigned)q;
   }
   const unsigned count = MIN2(den, sat), factor = maxf * count / sat;
   return (uint8_t)(((unsigned)pre * (256 - factor) + prob * factor + 128) >> 8);
}

static unsigned
merge_probs(const int8_t *tree, int i, uint8_t *probs, const unsigned *counts,
            unsigned sat, unsigned maxf)
{
   const int sl = tree[i], sr = tree[i + 1];
   const unsigned l = sl <= 0 ? counts[-sl] : merge_probs(tree, sl, probs, counts, sat, maxf);
   const unsigned r = sr <= 0 ? counts[-sr] : merge_probs(tree, sr, probs, counts, sat, maxf);
   probs[i >> 1] = merge_prob(probs[i >> 1], l, r, sat, maxf);
   return l + r;
}

static void
adapt_probs(const int8_t *tree, uint8_t *probs, const unsigned *counts)
{
   merge_probs(tree, 0, probs, counts, 20, 128);
}

static uint8_t
adapt_prob(uint8_t p, const unsigned c[2])
{
   return merge_prob(p, c[0], c[1], 20, 128);
}

static const int8_t intra_mode_tree[18] = { -0, 2, -9, 4, -1, 6, 8, 12, -2, 10, -4, -5, -3, 14, -8, 16, -6, -7 };
static const int8_t partition_tree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t interp_filter_tree[4] = { -VP9_EIGHTTAP, 2, -VP9_EIGHTTAP_SMOOTH, -VP9_EIGHTTAP_SHARP };
static const int8_t tx_size_32_tree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t tx_size_16_tree[4] = { -0, 2, -1, -2 };
static const int8_t tx_size_8_tree[2] = { -0, -1 };
/* values: NEARESTMV 0, NEARMV 1, ZEROMV 2, NEWMV 3 */
static const int8_t inter_mode_tree[6] = { -2, 2, -0, 4, -1, -3 };
static const int8_t mv_joint_tree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t mv_class_tree[20] = { -0, 2, -1, 4, 6, 8, -2, -3, 10, 12, -4, -5, -6, 14, 16, 18, -7, -8, -9, -10 };
static const int8_t mv_fr_tree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t small_token_tree[6] = { 0, 0, -0, 4, -1, -2 };

static void
adapt_coef(struct nouveau_vp9_probs *p, const nvdec_vp9EntropyCounts_t *c, unsigned uf)
{
   for (int t = 0; t < 4; t++) {
      const unsigned (*cc)[2][6][6][4] = t == 0 ? c->countCoeffs :
                                         t == 1 ? c->countCoeffs8x8 :
                                         t == 2 ? c->countCoeffs16x16 : c->countCoeffs32x32;
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 2; j++)
            for (int k = 0; k < 6; k++)
               for (int l = 0; l < (k == 0 ? 3 : 6); l++) {
                  /* NVDEC's counts: [0] ZERO_TOKEN, [1] ONE_TOKEN, [2] TWO_TOKEN
                   * or more, [3] no more coefficients; countEobs is the number
                   * of times more_coefs was read. */
                  const unsigned *n = cc[i][j][k][l];
                  const unsigned tok[3] = { n[0], n[1], n[2] };
                  const unsigned eob = c->countEobs[t][i][j][k][l];
                  const unsigned more[2] = { n[3], eob > n[3] ? eob - n[3] : 0 };
                  uint8_t *pr = p->coef[t][i][j][k][l];
                  merge_probs(small_token_tree, 2, pr, tok, 24, uf);
                  pr[0] = merge_prob(pr[0], more[0], more[1], 24, uf);
               }
   }
}

static void
adapt_noncoef(struct nouveau_vp9_probs *p, const struct nouveau_vp9_frame *h,
              const nvdec_vp9EntropyCounts_t *c)
{
   for (int i = 0; i < 4; i++)
      p->is_inter[i] = adapt_prob(p->is_inter[i], c->intra_inter_count[i]);
   for (int i = 0; i < 5; i++)
      p->comp_mode[i] = adapt_prob(p->comp_mode[i], c->comp_inter_count[i]);
   for (int i = 0; i < 5; i++)
      p->comp_ref[i] = adapt_prob(p->comp_ref[i], c->comp_ref_count[i]);
   for (int i = 0; i < 5; i++)
      for (int j = 0; j < 2; j++)
         p->single_ref[i][j] = adapt_prob(p->single_ref[i][j], c->single_ref_count[i][j]);
   for (int i = 0; i < 7; i++) {
      /* NVDEC counts the inter mode tree's branches: [0] ZEROMV / other,
       * [1] NEARESTMV / other, [2] NEARMV / NEWMV. */
      const unsigned v[4] = { c->inter_mode_counts[i][1][0], c->inter_mode_counts[i][2][0],
                              c->inter_mode_counts[i][0][0], c->inter_mode_counts[i][2][1] };
      adapt_probs(inter_mode_tree, p->inter_mode[i], v);
   }
   for (int i = 0; i < 4; i++)
      adapt_probs(intra_mode_tree, p->y_mode[i], c->sb_ymode_counts[i]);
   for (int i = 0; i < 10; i++)
      adapt_probs(intra_mode_tree, p->uv_mode[i], c->uv_mode_counts[i]);
   for (int i = 0; i < 16; i++)
      adapt_probs(partition_tree, p->partition[i], c->partition_counts[i]);
   for (int i = 0; i < 3; i++)
      p->skip[i] = adapt_prob(p->skip[i], c->mbskip_count[i]);
   if (h->interp_filter == VP9_SWITCHABLE)
      for (int i = 0; i < 4; i++)
         adapt_probs(interp_filter_tree, p->interp_filter[i], c->switchable_interp_counts[i]);
   if (h->tx_mode == VP9_TX_MODE_SELECT)
      for (int i = 0; i < 2; i++) {
         adapt_probs(tx_size_8_tree, p->tx8[i], c->tx8x8_count[i]);
         adapt_probs(tx_size_16_tree, p->tx16[i], c->tx16x16_count[i]);
         adapt_probs(tx_size_32_tree, p->tx32[i], c->tx32x32_count[i]);
      }
   const nvdec_nmv_context_counts *m = &c->nmvcount;
   adapt_probs(mv_joint_tree, p->mv_joint, m->joints);
   for (int i = 0; i < 2; i++) {
      p->mv_sign[i] = adapt_prob(p->mv_sign[i], m->sign[i]);
      adapt_probs(mv_class_tree, p->mv_class[i], m->classes[i]);
      p->mv_class0_bit[i] = adapt_prob(p->mv_class0_bit[i], m->class0[i]);
      for (int j = 0; j < 10; j++)
         p->mv_bits[i][j] = adapt_prob(p->mv_bits[i][j], m->bits[i][j]);
      for (int j = 0; j < 2; j++)
         adapt_probs(mv_fr_tree, p->mv_class0_fr[i][j], m->class0_fp[i][j]);
      adapt_probs(mv_fr_tree, p->mv_fr[i], m->fp[i]);
      if (h->allow_high_precision_mv) {
         p->mv_class0_hp[i] = adapt_prob(p->mv_class0_hp[i], m->class0_hp[i]);
         p->mv_hp[i] = adapt_prob(p->mv_hp[i], m->hp[i]);
      }
   }
}

void
nouveau_vp9_end_frame(struct nouveau_vp9_state *state,
                      const struct nouveau_vp9_frame *frame,
                      const void *counts)
{
   if (frame->show_existing_frame)
      return;
   if (nouveau_vp9_adapts(frame) && counts) {
      /* refresh_probs (6.1.2): load_probs, adapt the coefficients; for inter
       * frames load_probs2 and adapt the rest. */
      const struct nouveau_vp9_probs *pre = &state->ctx[frame->frame_context_idx];
      struct nouveau_vp9_probs adapted = *pre;
      memcpy(adapted.tx8, state->fc.tx8, sizeof(adapted.tx8));
      memcpy(adapted.tx16, state->fc.tx16, sizeof(adapted.tx16));
      memcpy(adapted.tx32, state->fc.tx32, sizeof(adapted.tx32));
      memcpy(adapted.skip, state->fc.skip, sizeof(adapted.skip));
      adapt_coef(&adapted, counts,
                 frame->frame_is_intra ? 112 :
                 frame->last_frame_type == VP9_KEY_FRAME ? 128 : 112);
      if (!frame->frame_is_intra) {
         memcpy(adapted.tx8, pre->tx8, sizeof(adapted.tx8));
         memcpy(adapted.tx16, pre->tx16, sizeof(adapted.tx16));
         memcpy(adapted.tx32, pre->tx32, sizeof(adapted.tx32));
         memcpy(adapted.skip, pre->skip, sizeof(adapted.skip));
         adapt_noncoef(&adapted, frame, counts);
      }
      state->fc = adapted;
   }
   if (frame->refresh_frame_context)
      state->ctx[frame->frame_context_idx] = state->fc;
}

void
nouveau_vp9_pack_probs(void *table, const struct nouveau_vp9_state *state,
                       const struct nouveau_vp9_frame *frame)
{
   nvdec_vp9EntropyProbs_t *e = table;
   const struct nouveau_vp9_probs *p = &state->fc;
   memset(e, 0, sizeof(*e));
   /* Key frame y/uv mode probabilities split 8 + 1. */
   for (int i = 0; i < 10; i++)
      for (int j = 0; j < 10; j++) {
         memcpy(e->kf_bmode_prob[i][j], kf_y_mode_probs[i][j], 8);
         e->kf_bmode_probB[i][j][0] = kf_y_mode_probs[i][j][8];
      }
   for (int i = 0; i < 10; i++) {
      memcpy(e->kf_uv_mode_prob[i], kf_uv_mode_probs[i], 8);
      e->kf_uv_mode_probB[i][0] = kf_uv_mode_probs[i][8];
   }
   if (frame->seg_enabled && frame->seg_update_map) {
      memcpy(e->mb_segment_tree_probs, frame->seg_tree_probs, 7);
      memcpy(e->segment_pred_probs, frame->seg_pred_probs, 3);
   } else {
      memset(e->mb_segment_tree_probs, 255, 7);
      memset(e->segment_pred_probs, 255, 3);
   }
   nvdec_vp9AdaptiveEntropyProbs_t *a = &e->a;
   for (int i = 0; i < 7; i++)
      memcpy(a->inter_mode_prob[i], p->inter_mode[i], 3);
   memcpy(a->intra_inter_prob, p->is_inter, 4);
   for (int i = 0; i < 10; i++) {
      memcpy(a->uv_mode_prob[i], p->uv_mode[i], 8);
      a->uv_mode_probB[i][0] = p->uv_mode[i][8];
   }
   for (int i = 0; i < 2; i++) {
      a->tx8x8_prob[i][0] = p->tx8[i][0];
      memcpy(a->tx16x16_prob[i], p->tx16[i], 2);
      memcpy(a->tx32x32_prob[i], p->tx32[i], 3);
   }
   for (int i = 0; i < 4; i++) {
      memcpy(a->sb_ymode_prob[i], p->y_mode[i], 8);
      a->sb_ymode_probB[i][0] = p->y_mode[i][8];
   }
   /* Key frame partition probabilities in [0], inter frame ones in [1]. */
   for (int i = 0; i < 16; i++) {
      memcpy(a->partition_prob[0][i], kf_partition_probs[i], 3);
      memcpy(a->partition_prob[1][i], p->partition[i], 3);
   }
   memcpy(a->switchable_interp_prob, p->interp_filter, sizeof(a->switchable_interp_prob));
   memcpy(a->comp_inter_prob, p->comp_mode, 5);
   memcpy(a->mbskip_probs, p->skip, 3);
   nvdec_nmv_context *m = &a->nmvc;
   memcpy(m->joints, p->mv_joint, 3);
   for (int i = 0; i < 2; i++) {
      m->sign[i] = p->mv_sign[i];
      m->class0[i][0] = p->mv_class0_bit[i];
      memcpy(m->fp[i], p->mv_fr[i], 3);
      m->class0_hp[i] = p->mv_class0_hp[i];
      m->hp[i] = p->mv_hp[i];
      memcpy(m->classes[i], p->mv_class[i], 10);
      memcpy(m->class0_fp[i], p->mv_class0_fr[i], sizeof(m->class0_fp[i]));
      memcpy(m->bits[i], p->mv_bits[i], 10);
   }
   memcpy(a->single_ref_prob, p->single_ref, sizeof(a->single_ref_prob));
   memcpy(a->comp_ref_prob, p->comp_ref, 5);
   unsigned char (*coef[4])[2][6][6][4] = {
      a->probCoeffs, a->probCoeffs8x8, a->probCoeffs16x16, a->probCoeffs32x32,
   };
   for (int t = 0; t < 4; t++)
      for (int i = 0; i < 2; i++)
         for (int j = 0; j < 2; j++)
            for (int k = 0; k < 6; k++)
               for (int l = 0; l < 6; l++)
                  memcpy(coef[t][i][j][k][l], p->coef[t][i][j][k][l], 3);
}

unsigned
nouveau_vp9_filter_size(unsigned width, unsigned height)
{
   /* Deblocking top and left lines (nvdec_drv.h's sizes), then the filter
    * "above" and BSD control areas, generously sized for 64 tile columns. */
   unsigned top = NVDEC_ALIGN256((align(width, 64) / 64 + 2) * 2048);
   unsigned left = NVDEC_ALIGN256((align(height, 64) / 64 + 2) * 1664);
   unsigned size = NVDEC_ALIGN256((uint64_t)top + left);
   size += NVDEC_ALIGN256((uint64_t)height * GIP_ASIC_VERT_FILTER_RAM_SIZE * 64);
   return size + NVDEC_ALIGN256((uint64_t)height * GIP_ASIC_BSD_CTRL_RAM_SIZE * 64);
}

unsigned
nouveau_vp9_seg_size(unsigned width, unsigned height)
{
   return NVDEC_ALIGN256((uint64_t)(align(width, 64) / 64) *
                         (align(height, 64) / 64) * GIP_ASIC_VP9_CTB_SEG_SIZE);
}

unsigned
nouveau_vp9_colmv_size(unsigned width, unsigned height)
{
   /* 1024 bytes per superblock, measured on the engine. */
   return NVDEC_ALIGN256((uint64_t)(align(width, 64) / 64) *
                         (align(height, 64) / 64) * 1024);
}
