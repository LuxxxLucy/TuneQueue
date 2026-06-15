#ifndef FRAMES_H
#define FRAMES_H

#include "raylib.h"

void frames_init(const char *cache_dir, const char *mpv_path,
                 const char *ytdl_path);
void frames_shutdown(void);
// grab a still from video_id at position seconds (supersedes any pending one)
void frames_request(const char *video_id, double position);
// latest decoded still for video_id, or NULL
Texture2D *frames_get(const char *video_id);
// upload a finished grab to a GL texture; call once per frame on the main
// thread
void frames_pump(void);

#endif
