#include <stddef.h>
#include <string.h>

#include "nouveau_nvenc_h264.h"

_Static_assert(NV_NVENC_DRV_MAGIC_VALUE == 0xc7b70006, "NVENC 7.3/C7B7 magic");
_Static_assert(sizeof(nvenc_h264_surface_cfg_s) == 32, "surface configuration ABI");
_Static_assert(sizeof(nvenc_h264_drv_pic_setup_s) == 512, "picture setup ABI");
_Static_assert(sizeof(nvenc_h264_slice_control_s) == 128, "slice setup ABI");
_Static_assert(sizeof(nvenc_pic_stat_s) == 128, "picture feedback ABI");
_Static_assert(offsetof(struct nouveau_nvenc_h264_config, slice) ==
               NOUVEAU_NVENC_H264_PICTURE_RESERVED_BYTES, "reserved picture prefix");

unsigned
nouveau_nvenc_h264_level(unsigned width, unsigned height,
                         unsigned frame_rate_num, unsigned frame_rate_den)
{
   static const struct {
      uint8_t idc;
      uint32_t max_mbps;
      uint32_t max_fs;
   } levels[] = {
      { 10, 1485, 99 },       { 11, 3000, 396 },      { 12, 6000, 396 },
      { 20, 11880, 396 },     { 21, 19800, 792 },
      { 22, 20250, 1620 },    { 30, 40500, 1620 },    { 31, 108000, 3600 },
      { 32, 216000, 5120 },   { 40, 245760, 8192 },   { 41, 245760, 8192 },
      { 42, 522240, 8704 },   { 50, 589824, 22080 },  { 51, 983040, 36864 },
      { 52, 2073600, 36864 },
   };
   if (!frame_rate_num || !frame_rate_den) {
      frame_rate_num = 30;
      frame_rate_den = 1;
   }
   const uint64_t mb_w = (width + 15) / 16, mb_h = (height + 15) / 16;
   const uint64_t frame_size = mb_w * mb_h;
   const uint64_t rate =
      (frame_size * frame_rate_num + frame_rate_den - 1) / frame_rate_den;

   for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
      if (frame_size <= levels[i].max_fs && rate <= levels[i].max_mbps &&
          mb_w * mb_w <= 8ull * levels[i].max_fs &&
          mb_h * mb_h <= 8ull * levels[i].max_fs)
         return levels[i].idc;
   }
   return 52;
}

bool
nouveau_nvenc_h264_idr_config(struct nouveau_nvenc_h264_config *config,
                              unsigned width, unsigned height, unsigned qp,
                              unsigned bitstream_bytes, unsigned history_bytes,
                              bool force_ipcm)
{
   if (!config || width < 128 || height < 96 || width > 1920 || height > 1088 ||
       (width & 15) || (height & 15) || qp > 51 ||
       bitstream_bytes < (uint64_t)width * height * 2 + 4096 ||
       history_bytes < (uint64_t)(width / 16) * (height / 16) * H264_HIST_BLOCK_SIZE + 4096)
      return false;

   memset(config, 0, sizeof(*config));
   nvenc_h264_drv_pic_setup_s *picture = &config->picture;
   picture->magic = NV_NVENC_DRV_MAGIC_VALUE;
   nvenc_h264_surface_cfg_s surface = {
      .frame_width_minus1 = width - 1,
      .frame_height_minus1 = height - 1,
      .sfc_pitch = width,
      .sfc_pitch_chroma = width,
      .tiled_16x16 = 1,
   };
   picture->input_cfg = surface;
   picture->refpic_cfg = surface;
   picture->outputpic_cfg = surface;
   picture->sps_data.profile_idc = 66;
   picture->sps_data.level_idc = 40;
   picture->sps_data.chroma_format_idc = 1;
   picture->sps_data.pic_order_cnt_type = 2;
   picture->sps_data.frame_mbs_only = 1;
   picture->pps_data.pic_init_qp_minus26 = (int)qp - 26;
   picture->pps_data.deblocking_filter_control_present_flag = 1;
   picture->pic_control.pic_type = 3;
   picture->pic_control.ref_pic_flag = 1;
   picture->pic_control.slice_mode = 1;
   picture->pic_control.codec = 3;
   picture->pic_control.strips_in_frame = 1;
   picture->pic_control.slice_encoding_row_num = height / 16;
   picture->pic_control.bitstream_buf_size = bitstream_bytes;
   picture->pic_control.hist_buf_size = history_bytes;
   picture->pic_control.slice_control_offset = offsetof(struct nouveau_nvenc_h264_config, slice);
   picture->pic_control.me_control_offset = offsetof(struct nouveau_nvenc_h264_config, me);
   picture->pic_control.md_control_offset = offsetof(struct nouveau_nvenc_h264_config, md);
   picture->pic_control.q_control_offset = offsetof(struct nouveau_nvenc_h264_config, quant);
   picture->pic_control.wp_control_offset = offsetof(struct nouveau_nvenc_h264_config, weights);
   for (unsigned i = 0; i < 8; i++) {
      picture->pic_control.l0[i] = -1;
      picture->pic_control.l1[i] = -1;
   }
   for (unsigned i = 0; i < 3; i++) {
      picture->rate_control.QP[i] = qp;
      picture->rate_control.minQP[i] = qp;
      picture->rate_control.maxQP[i] = qp;
      picture->rate_control.rhopbi[i] = 1 << 8;
   }
   picture->rate_control.framerate = 24;
   picture->rate_control.buffersize = bitstream_bytes;
   picture->rate_control.gop_length = 1;
   picture->gpTimer_timeout_val = 81000000;
   config->slice.num_mb = (width / 16) * (height / 16);
   config->slice.qp_avr = qp;
   config->slice.qp_slice_min = qp;
   config->slice.qp_slice_max = qp;
   config->slice.force_intra = 1;
   config->slice.disable_deblocking_filter_idc = 0;
   config->md.intra_luma4x4_mode_enable = 0x1ff;
   config->md.intra_luma16x16_mode_enable = 0xf;
   config->md.intra_chroma_mode_enable = 0xf;
   config->md.early_intra_mode_control = 3;
   config->md.ip_search_mode = 5;
   config->md.force_ipcm = force_ipcm;
   config->me.hint_type0 = 0;
   config->me.hint_type1 = 1;
   config->me.hint_type2 = 2;
   config->me.hint_type3 = 3;
   config->me.hint_type4 = 4;
   return true;
}

bool
nouveau_nvenc_h264_idr_feedback(const nvenc_pic_stat_s *feedback,
                                bool completion_signalled, uint32_t picture_index,
                                unsigned bitstream_bytes, unsigned *output_bytes)
{
   if (!feedback || !output_bytes || !completion_signalled ||
       feedback->picture_index != picture_index || feedback->error_status ||
       feedback->ucode_error_status ||
       (feedback->pic_type != 3 && feedback->pic_type != 0) ||
       feedback->num_slices != 1 || !feedback->total_bit_count ||
       (uint64_t)feedback->total_bit_count > (uint64_t)bitstream_bytes * 8)
      return false;
   *output_bytes = ((uint64_t)feedback->total_bit_count + 7) / 8;
   return true;
}

struct nvenc_h264_rbsp {
   uint8_t data[32];
   unsigned bits;
   bool overflow;
};

static void
nvenc_h264_bits(struct nvenc_h264_rbsp *rbsp, unsigned value, unsigned count)
{
   while (count) {
      count--;
      if (rbsp->bits == sizeof(rbsp->data) * 8) {
         rbsp->overflow = true;
         return;
      }
      rbsp->data[rbsp->bits / 8] |= ((value >> count) & 1) << (7 - rbsp->bits % 8);
      rbsp->bits++;
   }
}

static void
nvenc_h264_ue(struct nvenc_h264_rbsp *rbsp, unsigned value)
{
   unsigned code = value + 1;
   unsigned length = 1;
   for (unsigned n = code; n > 1; n >>= 1)
      length++;
   nvenc_h264_bits(rbsp, 0, length - 1);
   nvenc_h264_bits(rbsp, code, length);
}

static void
nvenc_h264_se(struct nvenc_h264_rbsp *rbsp, int value)
{
   nvenc_h264_ue(rbsp, value <= 0 ? -2 * value : 2 * value - 1);
}

static size_t
nvenc_h264_nal(struct nvenc_h264_rbsp *rbsp, uint8_t header, uint8_t *data)
{
   nvenc_h264_bits(rbsp, 1, 1);
   while (rbsp->bits % 8)
      nvenc_h264_bits(rbsp, 0, 1);
   if (rbsp->overflow)
      return 0;

   data[0] = 0;
   data[1] = 0;
   data[2] = 0;
   data[3] = 1;
   data[4] = header;
   size_t size = 5;
   unsigned zeros = 0;
   for (unsigned i = 0; i < rbsp->bits / 8; i++) {
      uint8_t byte = rbsp->data[i];
      if (zeros == 2 && byte <= 3) {
         data[size++] = 3;
         zeros = 0;
      }
      data[size++] = byte;
      zeros = byte == 0 ? zeros + 1 : 0;
   }
   return size;
}

bool
nouveau_nvenc_h264_idr_headers(const struct nouveau_nvenc_h264_config *config,
                               uint8_t *data, size_t capacity, size_t *size)
{
   if (!config || !data || !size)
      return false;
   const nvenc_h264_drv_pic_setup_s *picture = &config->picture;
   const nvenc_h264_sps_data_s *sps = &picture->sps_data;
   const nvenc_h264_pps_data_s *pps = &picture->pps_data;
   unsigned width = picture->input_cfg.frame_width_minus1 + 1;
   unsigned height = picture->input_cfg.frame_height_minus1 + 1;
   if (picture->magic != NV_NVENC_DRV_MAGIC_VALUE ||
       width < 128 || width > 1920 || height < 96 || height > 1088 ||
       (width & 15) || (height & 15) || sps->profile_idc != 66 || sps->level_idc != 40 ||
       sps->chroma_format_idc != 1 || sps->pic_order_cnt_type != 2 ||
       sps->log2_max_frame_num_minus4 || !sps->frame_mbs_only ||
       sps->stereo_mvc_enable || sps->separate_colour_plane_flag ||
       sps->lossless_qpprime_flag || pps->pic_param_set_id || pps->entropy_coding_mode_flag ||
       pps->num_ref_idx_l0_active_minus1 || pps->num_ref_idx_l1_active_minus1 ||
       pps->weighted_bipred_idc || pps->weighted_pred_flag || pps->pic_order_present_flag ||
       pps->chroma_qp_index_offset || pps->second_chroma_qp_index_offset ||
       pps->transform_8x8_mode_flag || pps->pic_init_qp_minus26 < -26 ||
       pps->pic_init_qp_minus26 > 25 || picture->pic_control.bit_depth_minus_8 ||
       picture->pic_control.pic_type != 3 || picture->pic_control.pic_struct ||
       picture->pic_control.frame_num || picture->pic_control.codec != 3 ||
       picture->refpic_cfg.frame_width_minus1 != width - 1 ||
       picture->refpic_cfg.frame_height_minus1 != height - 1 ||
       picture->outputpic_cfg.frame_width_minus1 != width - 1 ||
       picture->outputpic_cfg.frame_height_minus1 != height - 1)
      return false;

   struct nvenc_h264_rbsp seq = {0}, pic = {0};
   nvenc_h264_bits(&seq, 66, 8);
   nvenc_h264_bits(&seq, 0xc0, 8);
   nvenc_h264_bits(&seq, 40, 8);
   nvenc_h264_ue(&seq, 0);
   nvenc_h264_ue(&seq, 0);
   nvenc_h264_ue(&seq, 2);
   nvenc_h264_ue(&seq, 1);
   nvenc_h264_bits(&seq, 0, 1);
   nvenc_h264_ue(&seq, width / 16 - 1);
   nvenc_h264_ue(&seq, height / 16 - 1);
   nvenc_h264_bits(&seq, 1, 1);
   nvenc_h264_bits(&seq, 1, 1);
   nvenc_h264_bits(&seq, 0, 1);
   nvenc_h264_bits(&seq, 0, 1);

   nvenc_h264_ue(&pic, 0);
   nvenc_h264_ue(&pic, 0);
   nvenc_h264_bits(&pic, 0, 1);
   nvenc_h264_bits(&pic, 0, 1);
   nvenc_h264_ue(&pic, 0);
   nvenc_h264_ue(&pic, 0);
   nvenc_h264_ue(&pic, 0);
   nvenc_h264_bits(&pic, 0, 1);
   nvenc_h264_bits(&pic, 0, 2);
   nvenc_h264_se(&pic, pps->pic_init_qp_minus26);
   nvenc_h264_se(&pic, 0);
   nvenc_h264_se(&pic, 0);
   nvenc_h264_bits(&pic, pps->deblocking_filter_control_present_flag, 1);
   nvenc_h264_bits(&pic, pps->constrained_intra_pred_flag, 1);
   nvenc_h264_bits(&pic, 0, 1);

   uint8_t output[128];
   size_t seq_size = nvenc_h264_nal(&seq, 0x67, output);
   size_t pic_size = nvenc_h264_nal(&pic, 0x68, output + seq_size);
   if (!seq_size || !pic_size || capacity < seq_size + pic_size)
      return false;
   memcpy(data, output, seq_size + pic_size);
   *size = seq_size + pic_size;
   return true;
}
