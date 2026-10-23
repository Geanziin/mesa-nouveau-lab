/* SPDX-License-Identifier: MIT */
#include "nouveau_nvdec_av1.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   uint8_t guarded[NVDEC_AV1_CDF_SIZE + 32];
   for (unsigned q = 0; q < 4; q++) {
      memset(guarded, 0xa5, sizeof(guarded));
      nouveau_nvdec_av1_default_cdf(q, guarded + 16);
      for (unsigned i = 0; i < 16; i++) {
         assert(guarded[i] == 0xa5);
         assert(guarded[16 + NVDEC_AV1_CDF_SIZE + i] == 0xa5);
      }
      if (fwrite(guarded + 16, 1, NVDEC_AV1_CDF_SIZE, stdout) != NVDEC_AV1_CDF_SIZE)
         return 1;
   }
   return 0;
}
