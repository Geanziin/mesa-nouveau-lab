/*
 * SPDX-License-Identifier: MIT
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 * libnvcuvid.so.615.78.08 SHA-256
 * 6a3595cd70645bf07cf4af5a91d709d1a91bc940dd8cb0a82868ca8be0967a5e
 */
#include <errno.h>
#include <string.h>

#include "nouveau_nvenc_backend.h"
#include "nouveau_nvenc_h264.h"
#include "nouveau_nvenc_tables.h"

int
nvenc_make_config(unsigned width, unsigned height, unsigned qp,
                  struct nouveau_nvenc_h264_config *output)
{
   if (!output || qp > 51 || width < 128 || width > 1920 || height < 96 ||
       height > 1088 || (width & 15) || (height & 15))
      return -EINVAL;
   struct nouveau_nvenc_h264_config config;
   if (!nouveau_nvenc_h264_idr_config(&config, width, height, qp, 4u << 20,
                                      4u << 20, false))
      return -EIO;
   const unsigned mb_w = width / 16, mb = mb_w * (height / 16);
   config.picture.pic_control.hist_buf_size = (mb_w * 384u + 255u) & ~255u;
   config.picture.gpTimer_timeout_val = 46080u * (mb > 3600u ? mb : 3600u);
   unsigned char *bytes = (unsigned char *)&config;
   for (unsigned r = 0; r < nvenc_overlay_run_count; r++)
      memcpy(bytes + nvenc_overlay_runs[r].offset,
             nvenc_overlay_runs[r].bytes, nvenc_overlay_runs[r].length);
   *output = config;
   return 0;
}
