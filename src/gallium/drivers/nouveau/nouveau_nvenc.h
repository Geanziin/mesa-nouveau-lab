/*
 * SPDX-License-Identifier: MIT
 * NVIDIA-DERIVED: private local use only, not for redistribution/upstream.
 */
#ifndef NOUVEAU_NVENC_ENCODER_H
#define NOUVEAU_NVENC_ENCODER_H
#include "pipe/p_video_codec.h"
struct nouveau_screen;
int nouveau_nvenc_param(struct nouveau_screen *, enum pipe_video_profile,
                        enum pipe_video_cap);
struct pipe_video_codec *
nouveau_nvenc_create(struct pipe_context *, const struct pipe_video_codec *);
#endif
