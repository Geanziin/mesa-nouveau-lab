/* SPDX-License-Identifier: MIT
 * Native Gallium video submission. Like radeonsi's multimedia backend, this
 * owns a video channel and submits firmware commands through the DRM winsys;
 * VA-API does not create a Vulkan instance/device or call another graphics API.
 */
#define MESA_LOG_TAG "nouveau-nvdec"

#include "nouveau_nvdec.h"
#include "nouveau_nvdec_h264.h"
#include "nouveau_screen.h"
#include "nvc0/nvc0_resource.h"
#include "nvdec_drv.h"
#include "nvif/class.h"
#include "clc5b0.h"
#include "clc7b0.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "vl/vl_video_buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define NVDEC_PICTURES 17
#define NVDEC_SUBC 2
#define NVDEC_TIMEOUT_NS 5000000000ll

enum nvdec_buffer {
   NVDEC_PICTURE,
   NVDEC_OFFSETS,
   NVDEC_STATUS,
   NVDEC_FENCE,
   NVDEC_COLOC,
   NVDEC_MBHIST,
   NVDEC_HISTORY,
   NVDEC_BITSTREAM,
   NVDEC_BUFFERS,
};

struct nvdec_surface {
   struct pipe_resource *planes[2];
};

struct nouveau_nvdec {
   struct pipe_video_codec base;
   struct nouveau_screen *screen;
   struct nouveau_client *client;
   struct nouveau_object *channel, *engine;
   struct nouveau_pushbuf *push;
   struct nouveau_bo *buffers[NVDEC_BUFFERS];
   struct nvdec_surface surfaces[NVDEC_PICTURES];
   struct nouveau_nvdec_bitstream stream;
   uint32_t sequence;
   unsigned width, height;
   int error;
};

static int
open_channel(struct nouveau_screen *screen, struct nouveau_object **channel,
             struct nouveau_object **engine)
{
   struct nve0_fifo args = {.engine = NOUVEAU_FIFO_ENGINE_NVDEC};
   int ret = nouveau_object_new(&screen->device->object, 0,
                                NOUVEAU_FIFO_CHANNEL_CLASS, &args, sizeof(args), channel);
   if (ret)
      return ret;
   const struct nouveau_mclass classes[] = {
      {NVC7B0_VIDEO_DECODER, -1},
      {0, 0},
   };
   ret = nouveau_object_mclass(*channel, classes);
   if (ret >= 0)
      ret = nouveau_object_new(*channel, 0xbeefb0, classes[ret].oclass, NULL, 0, engine);
   if (ret)
      nouveau_object_del(channel);
   return ret;
}

bool
nouveau_nvdec_enabled(struct nouveau_screen *screen)
{
   /* Only the GA107 firmware/layout has been validated so far. This is not a
    * Vulkan capability check: the native backend probes the DRM video channel.
    */
   if (screen->device->chipset != 0x177 ||
       !debug_get_bool_option("NOUVEAU_NVDEC", true))
      return false;

   simple_mtx_lock(&screen->fence.lock);
   if (!screen->nvdec.probed) {
      struct nouveau_object *channel = NULL, *engine = NULL;
      screen->nvdec.supported = open_channel(screen, &channel, &engine) == 0;
      nouveau_object_del(&engine);
      nouveau_object_del(&channel);
      screen->nvdec.probed = true;
   }
   bool supported = screen->nvdec.supported;
   simple_mtx_unlock(&screen->fence.lock);
   return supported;
}

int
nouveau_nvdec_get_param(struct nouveau_screen *screen,
                        enum pipe_video_profile profile,
                        enum pipe_video_entrypoint entrypoint,
                        enum pipe_video_cap param)
{
   if (entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM ||
       (profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE &&
        profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE &&
        profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN &&
        profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH) || !nouveau_nvdec_enabled(screen))
      return 0;
   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTED:
   case PIPE_VIDEO_CAP_SUPPORTS_PROGRESSIVE:
      return 1;
   case PIPE_VIDEO_CAP_MIN_WIDTH:
      return 48;
   case PIPE_VIDEO_CAP_MIN_HEIGHT:
      return 16;
   case PIPE_VIDEO_CAP_MAX_WIDTH:
   case PIPE_VIDEO_CAP_MAX_HEIGHT:
      return 4096;
   case PIPE_VIDEO_CAP_MAX_MACROBLOCKS:
      return 65536;
   default:
      return 0;
   }
}

static void
release_surface(struct nvdec_surface *surface)
{
   for (unsigned i = 0; i < 2; i++)
      pipe_resource_reference(&surface->planes[i], NULL);
}

static void
destroy_decoder(struct pipe_video_codec *codec)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   /* end_frame is synchronous, including its failure path. Destroy the channel
    * before dropping storage when a watchdog timeout poisoned the decoder.
    */
   nouveau_pushbuf_del(&dec->push);
   nouveau_object_del(&dec->engine);
   nouveau_object_del(&dec->channel);
   for (unsigned i = 0; i < NVDEC_PICTURES; i++)
      release_surface(&dec->surfaces[i]);
   for (unsigned i = 0; i < NVDEC_BUFFERS; i++)
      nouveau_bo_ref(NULL, &dec->buffers[i]);
   nouveau_client_del(&dec->client);
   free(dec->stream.data);
   free(dec);
}

static int
allocate_buffer(struct nouveau_nvdec *dec, unsigned index, size_t size)
{
   int ret = nouveau_bo_new(dec->screen->device,
                            NOUVEAU_BO_GART | NOUVEAU_BO_COHERENT | NOUVEAU_BO_MAP,
                            4096, align64(size, 4096), NULL, &dec->buffers[index]);
   if (ret)
      return ret;
   ret = nouveau_bo_map(dec->buffers[index], 0, dec->client);
   if (!ret)
      memset(dec->buffers[index]->map, 0, dec->buffers[index]->size);
   return ret;
}

static bool
get_surface(struct nouveau_nvdec *dec, struct pipe_video_buffer *buffer,
            struct pipe_resource *planes[3])
{
   if (!buffer || buffer->buffer_format != PIPE_FORMAT_NV12 || buffer->interlaced ||
       buffer->width < dec->width || buffer->height < dec->height ||
       buffer->context->screen != dec->base.context->screen)
      return false;
   buffer->get_resources(buffer, planes);
   if (!planes[0] || !planes[1])
      return false;
   for (unsigned i = 0; i < 2; i++) {
      struct nv50_miptree *mt = nv50_miptree(planes[i]);
      if (!mt->base.bo || mt->base.base.last_level || mt->base.base.nr_samples > 1 ||
          mt->base.base.width0 < (dec->width >> i) ||
          mt->base.base.height0 < (dec->height >> i) ||
          mt->base.base.format != (i ? PIPE_FORMAT_R8G8_UNORM : PIPE_FORMAT_R8_UNORM) ||
          mt->base.base.array_size != 1 || mt->level[0].tile_mode != 0x10 ||
          (mt->base.address + mt->level[0].offset) & 255 ||
          mt->level[0].pitch < dec->width ||
          mt->base.bo->config.nvc0.memtype != 0x06)
         return false;
   }
   return true;
}

static bool
same_surface(const struct nvdec_surface *surface, struct pipe_resource *const planes[3])
{
   return surface->planes[0] == planes[0] && surface->planes[1] == planes[1];
}

static bool
prepare_surfaces(struct nouveau_nvdec *dec, struct pipe_video_buffer *target,
                 const struct pipe_h264_picture_desc *picture,
                 unsigned *current, uint8_t references[16])
{
   bool keep[NVDEC_PICTURES] = {false};
   struct pipe_resource *planes[3] = {NULL};
   memset(references, 0xff, 16);
   for (unsigned i = 0; i < 16; i++) {
      if (!picture->ref[i])
         continue;
      if (!get_surface(dec, picture->ref[i], planes))
         return false;
      for (unsigned j = 0; j < NVDEC_PICTURES; j++) {
         if (!same_surface(&dec->surfaces[j], planes))
            continue;
         references[i] = j;
         keep[j] = true;
         break;
      }
      /* A reference must have been decoded in this session. In particular,
       * never guess its colocated-MV slot from the application's DPB order.
       */
      if (references[i] == 0xff)
         return false;
   }
   for (unsigned i = 0; i < NVDEC_PICTURES; i++)
      if (!keep[i])
         release_surface(&dec->surfaces[i]);
   if (!get_surface(dec, target, planes))
      return false;
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      if (keep[i] && same_surface(&dec->surfaces[i], planes))
         return false;
   }
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      if (keep[i])
         continue;
      for (unsigned p = 0; p < 2; p++)
         pipe_resource_reference(&dec->surfaces[i].planes[p], planes[p]);
      *current = i;
      return true;
   }
   return false;
}

static void
method(struct nouveau_pushbuf *push, unsigned address, uint32_t value)
{
   *push->cur++ = 0x20010000 | (NVDEC_SUBC << 13) | (address >> 2);
   *push->cur++ = value;
}

static uint32_t
buffer_address(struct nouveau_nvdec *dec, unsigned index)
{
   return dec->buffers[index]->offset >> 8;
}

static int
submit_frame(struct nouveau_nvdec *dec, unsigned current)
{
   struct nouveau_pushbuf_refn refs[NVDEC_BUFFERS + NVDEC_PICTURES * 2];
   unsigned count = 0;
   for (unsigned i = 0; i < NVDEC_BUFFERS; i++)
      refs[count++] = (struct nouveau_pushbuf_refn) {
         dec->buffers[i], NOUVEAU_BO_GART | NOUVEAU_BO_RDWR,
      };
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      for (unsigned p = 0; p < 2; p++) {
         struct pipe_resource *plane = dec->surfaces[i].planes[p];
         if (plane)
            refs[count++] = (struct nouveau_pushbuf_refn) {
               nv04_resource(plane)->bo,
               nv04_resource(plane)->domain | (i == current ? NOUVEAU_BO_WR : NOUVEAU_BO_RD),
            };
      }
   }
   int ret = nouveau_pushbuf_space(dec->push, 256, 0, 0);
   if (!ret)
      ret = nouveau_pushbuf_refn(dec->push, refs, count);
   if (ret)
      return ret;

   struct nouveau_pushbuf *push = dec->push;
   const uint32_t seq = ++dec->sequence ? dec->sequence : ++dec->sequence;
   *(uint32_t *)dec->buffers[NVDEC_FENCE]->map = 0;
   method(push, 0, NVC5B0_VIDEO_DECODER);
   method(push, NVC5B0_SET_APPLICATION_ID, NVC5B0_SET_APPLICATION_ID_ID_H264);
   method(push, NVC5B0_SET_CONTROL_PARAMS,
          3 | (1 << 4) | (1 << 6) | (1 << 13));
   method(push, NVC5B0_SET_DRV_PIC_SETUP_OFFSET, buffer_address(dec, NVDEC_PICTURE));
   method(push, NVC5B0_SET_IN_BUF_BASE_OFFSET, buffer_address(dec, NVDEC_BITSTREAM));
   method(push, NVC5B0_SET_PICTURE_INDEX, seq);
   method(push, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, buffer_address(dec, NVDEC_OFFSETS));
   method(push, NVC5B0_SET_COLOC_DATA_OFFSET, buffer_address(dec, NVDEC_COLOC));
   method(push, NVC5B0_SET_HISTORY_OFFSET, buffer_address(dec, NVDEC_HISTORY));
   method(push, NVC5B0_SET_NVDEC_STATUS_OFFSET, buffer_address(dec, NVDEC_STATUS));
   for (unsigned i = 0; i < NVDEC_PICTURES; i++) {
      for (unsigned p = 0; p < 2; p++) {
         struct pipe_resource *plane = dec->surfaces[i].planes[p];
         uint32_t address = plane ? (nv04_resource(plane)->address +
                                    nv50_miptree(plane)->level[0].offset) >> 8 : 0;
         method(push, (p ? NVC5B0_SET_PICTURE_CHROMA_OFFSET0 :
                           NVC5B0_SET_PICTURE_LUMA_OFFSET0) + i * 4, address);
      }
   }
   method(push, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, buffer_address(dec, NVDEC_MBHIST));
   method(push, NVC5B0_EXECUTE, 0);
   method(push, NVC5B0_SEMAPHORE_A, dec->buffers[NVDEC_FENCE]->offset >> 32);
   method(push, NVC5B0_SEMAPHORE_B, dec->buffers[NVDEC_FENCE]->offset);
   method(push, NVC5B0_SEMAPHORE_C, seq);
   method(push, NVC5B0_SEMAPHORE_D, 0);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
   ret = nouveau_pushbuf_kick(push);
   if (ret)
      return ret;

   /* Bound the wait; a bad bitstream must not spin the desktop forever. The
    * firmware watchdog is enabled as well. No software decoding fallback.
    */
   int64_t deadline = os_time_get_nano() + NVDEC_TIMEOUT_NS;
   uint32_t *fence = dec->buffers[NVDEC_FENCE]->map;
   while (__atomic_load_n(fence, __ATOMIC_ACQUIRE) != seq) {
      if (os_time_get_nano() >= deadline)
         return -ETIMEDOUT;
      os_time_sleep(50);
   }
   const nvdec_status_s *status = dec->buffers[NVDEC_STATUS]->map;
   if (status->error_status || status->mbs_in_error || status->slice_header_error_code) {
      mesa_loge("decode failed: status=%#x, bad MBs=%u, slice=%#x",
                status->error_status, status->mbs_in_error, status->slice_header_error_code);
      return -EIO;
   }
   return 0;
}

static void
begin_frame(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
            struct pipe_picture_desc *picture)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   dec->stream.size = 0;
   dec->stream.slices = 0;
   if (!target || !picture || picture->protected_playback)
      dec->error = -EINVAL;
}

static void
decode_bitstream(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
                 struct pipe_picture_desc *picture, unsigned count,
                 const void *const *data, const unsigned *sizes)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   if (!dec->error && !nouveau_nvdec_bitstream_append(&dec->stream, count, data, sizes))
      dec->error = -EINVAL;
}

static int
end_frame(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
          struct pipe_picture_desc *picture)
{
   struct nouveau_nvdec *dec = (struct nouveau_nvdec *)codec;
   if (dec->error)
      return dec->error;
   const struct pipe_h264_picture_desc *h264 = (const struct pipe_h264_picture_desc *)picture;
   unsigned current;
   uint8_t references[16];
   nvdec_h264_pic_s params;
   if (!h264 || !prepare_surfaces(dec, target, h264, &current, references))
      return dec->error = -EINVAL;
   struct nv50_miptree *y = nv50_miptree(dec->surfaces[current].planes[0]);
   struct nv50_miptree *uv = nv50_miptree(dec->surfaces[current].planes[1]);
   if (!nouveau_nvdec_h264_picture(h264, &dec->stream, dec->width, dec->height,
                                    y->level[0].pitch, uv->level[0].pitch,
                                    current, references, &params))
      return dec->error = -EINVAL;

   /* Flush pending Gallium uses before submitting on the independent video
    * channel. Waiting here preserves ordering even for in-process resources
    * whose Gallium fence has not yet reached the kernel reservation object.
    */
   struct pipe_fence_handle *fence = NULL;
   codec->context->flush(codec->context, &fence, 0);
   bool ready = fence && codec->context->screen->fence_finish(codec->context->screen,
                                                            codec->context, fence, NVDEC_TIMEOUT_NS);
   codec->context->screen->fence_reference(codec->context->screen, &fence, NULL);
   if (!ready)
      return dec->error = -ETIMEDOUT;
   if (!dec->buffers[NVDEC_BITSTREAM] ||
       dec->buffers[NVDEC_BITSTREAM]->size < dec->stream.size + 16) {
      nouveau_bo_ref(NULL, &dec->buffers[NVDEC_BITSTREAM]);
      int ret = allocate_buffer(dec, NVDEC_BITSTREAM, dec->stream.size + 16);
      if (ret)
         return dec->error = ret;
   }
   memcpy(dec->buffers[NVDEC_PICTURE]->map, &params, sizeof(params));
   memcpy(dec->buffers[NVDEC_OFFSETS]->map, dec->stream.offsets,
          (dec->stream.slices + 1) * sizeof(uint32_t));
   memcpy(dec->buffers[NVDEC_BITSTREAM]->map, dec->stream.data, dec->stream.size);
   memcpy((uint8_t *)dec->buffers[NVDEC_BITSTREAM]->map + dec->stream.size, params.eos, 16);
   memset(dec->buffers[NVDEC_STATUS]->map, 0, dec->buffers[NVDEC_STATUS]->size);
   int ret = submit_frame(dec, current);
   if (ret) {
      mesa_loge("native H.264 submission failed: %s", strerror(-ret));
      dec->error = ret;
   }
   return ret;
}

static void
flush_decoder(struct pipe_video_codec *codec)
{
   /* end_frame has already completed the video submission. */
}

struct pipe_video_codec *
nouveau_nvdec_create(struct pipe_context *context, const struct pipe_video_codec *templ)
{
   struct nouveau_screen *screen = nouveau_screen(context->screen);
   if (!nouveau_nvdec_get_param(screen, templ->profile, templ->entrypoint, PIPE_VIDEO_CAP_SUPPORTED) ||
       templ->max_references > 16 || templ->width < 48 || templ->width > 4096 ||
       templ->height < 16 || templ->height > 4096)
      return NULL;
   struct nouveau_nvdec *dec = calloc(1, sizeof(*dec));
   if (!dec)
      return NULL;
   dec->base = *templ;
   pipe_reference_init(&dec->base.reference, 1);
   dec->base.context = context;
   dec->base.destroy = destroy_decoder;
   dec->base.begin_frame = begin_frame;
   dec->base.decode_bitstream = decode_bitstream;
   dec->base.end_frame = end_frame;
   dec->base.flush = flush_decoder;
   dec->screen = screen;
   dec->width = align(templ->width, 16);
   dec->height = align(templ->height, 16);
   int ret = nouveau_client_new(screen->device, &dec->client);
   if (!ret)
      ret = open_channel(screen, &dec->channel, &dec->engine);
   if (!ret)
      ret = nouveau_pushbuf_new(dec->client, dec->channel, 2, 16384, &dec->push);
   const size_t sizes[] = {
      [NVDEC_PICTURE] = sizeof(nvdec_h264_pic_s),
      [NVDEC_OFFSETS] = (NOUVEAU_NVDEC_MAX_SLICES + 1) * sizeof(uint32_t),
      [NVDEC_STATUS] = 4096,
      [NVDEC_FENCE] = 4096,
      [NVDEC_COLOC] = align(align(dec->height / 16, 2) * (dec->width / 16) * 64 - 63, 256) * NVDEC_PICTURES,
      [NVDEC_MBHIST] = align((dec->width / 16) * 104, 256),
      [NVDEC_HISTORY] = align((dec->width / 16) * 0x300, 512),
   };
   for (unsigned i = 0; !ret && i < ARRAY_SIZE(sizes); i++)
      ret = allocate_buffer(dec, i, sizes[i]);
   if (ret) {
      mesa_loge("native H.264 initialization failed: %s", strerror(-ret));
      destroy_decoder(&dec->base);
      return NULL;
   }
   mesa_logd("H.264: native NVDEC, %ux%u, direct Gallium surfaces", dec->width, dec->height);
   return &dec->base;
}

struct pipe_video_buffer *
nouveau_nvdec_buffer_create(struct pipe_context *context, const struct pipe_video_buffer *templ)
{
   struct pipe_video_buffer native = *templ;
   native.flags |= NVC0_RESOURCE_FLAG_VIDEO;
   /* Video surfaces must use the uncompressed generic kind on every engine. */
   native.bind |= PIPE_BIND_SHARED;
   return vl_video_buffer_create(context, &native);
}
