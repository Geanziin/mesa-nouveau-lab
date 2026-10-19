/*
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "nouveau_nvenc_rc.h"

#define NVENC_RC_MAX_STEP 2

static int
clamp_int(int v, int lo, int hi)
{
   return v < lo ? lo : v > hi ? hi : v;
}

void
nvenc_rc_init(struct nvenc_rc *rc, enum nvenc_rc_mode mode,
              unsigned target_bps, unsigned peak_bps, unsigned frame_rate_num,
              unsigned frame_rate_den, unsigned vbv_bits, unsigned min_qp,
              unsigned max_qp, unsigned initial_qp)
{
   if (!frame_rate_num || !frame_rate_den) {
      frame_rate_num = 30;
      frame_rate_den = 1;
   }
   if (max_qp > 51 || !max_qp)
      max_qp = 51;
   if (min_qp > max_qp)
      min_qp = max_qp;

   rc->mode = mode;
   rc->min_qp = min_qp;
   rc->max_qp = max_qp;
   rc->qp = clamp_int(initial_qp, min_qp, max_qp);
   rc->target_bits = (double)target_bps * frame_rate_den / frame_rate_num;
   rc->peak_bits = (double)(peak_bps > target_bps ? peak_bps : target_bps) *
                   frame_rate_den / frame_rate_num;
   rc->buffer_bits = 0;
   rc->buffer_size = vbv_bits ? vbv_bits : rc->target_bits * 30;
}

unsigned
nvenc_rc_qp(const struct nvenc_rc *rc)
{
   return rc->qp;
}

void
nvenc_rc_update(struct nvenc_rc *rc, unsigned frame_bytes)
{
   if (rc->mode == NVENC_RC_CQP || rc->target_bits <= 0 || !frame_bytes)
      return;

   const double bits = (double)frame_bytes * 8;
   const double frame_error = log2(bits / rc->target_bits);

   rc->buffer_bits += bits - rc->target_bits;
   if (rc->buffer_bits > rc->buffer_size)
      rc->buffer_bits = rc->buffer_size;
   if (rc->buffer_bits < -rc->buffer_size)
      rc->buffer_bits = -rc->buffer_size;
   const double fullness = rc->buffer_bits / rc->buffer_size;

   double gain_frame = 0.5, gain_buffer = 1.0;
   if (rc->mode == NVENC_RC_VBR) {
      gain_frame = 0.25;
      gain_buffer = 0.5;
   }

   double step = 6.0 * (gain_frame * frame_error + gain_buffer * fullness);
   if (rc->mode == NVENC_RC_VBR && bits > rc->peak_bits)
      step += 6.0 * log2(bits / rc->peak_bits);

   const int delta =
      clamp_int((int)lround(step), -NVENC_RC_MAX_STEP, NVENC_RC_MAX_STEP);
   rc->qp = clamp_int(rc->qp + delta, rc->min_qp, rc->max_qp);
}
