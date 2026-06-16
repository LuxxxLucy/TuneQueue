#ifndef THUMBS_H
#define THUMBS_H

#include "raylib.h"

void thumbs_init(const char *cache_dir);
void thumbs_shutdown(void);
// texture for key (video id) at url, or NULL if not ready; enqueues a fetch
Texture2D *thumbs_get(const char *key, const char *url);
// upload finished downloads to GL textures; call once per frame on the main
// thread
void thumbs_pump(void);
// drop GL textures and re-queue cached thumbnails; call after the GL context
// loses its textures (display sleep / GPU switch)
void thumbs_reload(void);

#endif
