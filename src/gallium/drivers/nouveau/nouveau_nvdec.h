/* SPDX-License-Identifier: MIT */
#ifndef NOUVEAU_NVDEC_H
#define NOUVEAU_NVDEC_H

#include "pipe/p_video_codec.h"

struct nouveau_screen;

bool nouveau_nvdec_enabled(struct nouveau_screen *screen);
int nouveau_nvdec_get_param(struct nouveau_screen *screen,
                            enum pipe_video_profile profile,
                            enum pipe_video_entrypoint entrypoint,
                            enum pipe_video_cap param);
struct pipe_video_codec *nouveau_nvdec_create(struct pipe_context *context,
                                             const struct pipe_video_codec *templ);
struct pipe_video_buffer *nouveau_nvdec_buffer_create(struct pipe_context *context,
                                                     const struct pipe_video_buffer *templ);

#endif
