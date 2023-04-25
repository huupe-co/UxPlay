#ifndef UXPLAYLIB_H
#define UXPLAYLIB_H

#include <gst/gst.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uxplay_video_renderer_info {
    GstElement *appsrc, *pipeline, *sink;
    GstBus *bus;
};

struct uxplay_video_renderer_info get_uxplay_video_renderer_info();

#ifdef __cplusplus
}
#endif

#endif //UXPLAYLIB_H