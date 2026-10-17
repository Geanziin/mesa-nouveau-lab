/*
 * SPDX-License-Identifier: MIT
 */
#ifndef NOUVEAU_NVENC_RC_H
#define NOUVEAU_NVENC_RC_H

#include <stdbool.h>
#include <stdint.h>

enum nvenc_rc_mode {
   NVENC_RC_CQP,
   NVENC_RC_CBR,
   NVENC_RC_VBR,
};

struct nvenc_rc {
   enum nvenc_rc_mode mode;
   unsigned min_qp, max_qp;
   int qp;
   double target_bits;
   double peak_bits;
   double buffer_bits;
   double buffer_size;
};

void nvenc_rc_init(struct nvenc_rc *rc, enum nvenc_rc_mode mode,
                   unsigned target_bps, unsigned peak_bps,
                   unsigned frame_rate_num, unsigned frame_rate_den,
                   unsigned vbv_bits, unsigned min_qp, unsigned max_qp,
                   unsigned initial_qp);
unsigned nvenc_rc_qp(const struct nvenc_rc *rc);
void nvenc_rc_update(struct nvenc_rc *rc, unsigned frame_bytes);

#endif
