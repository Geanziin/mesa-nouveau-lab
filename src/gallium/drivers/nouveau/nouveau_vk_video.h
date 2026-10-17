#ifndef NOUVEAU_VK_VIDEO_H
#define NOUVEAU_VK_VIDEO_H

#include "pipe/p_video_codec.h"

struct nouveau_screen;

bool nouveau_vk_video_enabled(struct nouveau_screen *screen);
int nouveau_vk_video_get_param(struct nouveau_screen *screen,
                               enum pipe_video_profile profile,
                               enum pipe_video_entrypoint entrypoint,
                               enum pipe_video_cap param);
struct pipe_video_codec *nouveau_vk_video_create(struct pipe_context *context,
                                                const struct pipe_video_codec *templ);
struct pipe_video_buffer *nouveau_vk_video_buffer_create(struct pipe_context *context,
                                                        const struct pipe_video_buffer *templ);

#endif
