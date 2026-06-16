#ifndef TUNE_QUEUE_APP_H
#define TUNE_QUEUE_APP_H

#include "db.h"
#include "model.h"
#include "player.h"

#define TUNE_QUEUE_DEFAULT_HEARTBEAT_INTERVAL 5.0
#define TUNE_QUEUE_DEFAULT_DATA_DIR "Library/Application Support/TuneQueue"
#define TUNE_QUEUE_DEFAULT_CACHE_DIR "Library/Caches/TuneQueue"

struct tune_queue_app_config {
    struct {
        const char *db_path;
        const char *ytdl_path;
        double heartbeat_interval;
    } core;
    struct {
        const char *mpv_path;
        const char *gui_font_path;
        const char *cache_dir;
    } gui;
};

struct tune_queue_app_state {
    sqlite3 *db;
    struct player player;

    struct queue_list queue;
    struct history_list history;
    struct liked_list liked;
    struct stats stats;
    int dirty;

    struct video now;
    int has_now;
    long long session_id;  // >0 while a play session is recorded
    double watched;        // seconds actually playing this session
    double hb_accum;       // seconds since the last heartbeat write
    double heartbeat_interval;
};

enum tune_queue_app_event { TUNE_QUEUE_APP_NONE, TUNE_QUEUE_APP_TRACK_FAILED };

void app_init(struct tune_queue_app_state *a,
              const struct tune_queue_app_config *cfg);
void app_shutdown(struct tune_queue_app_state *a);

void app_reload(struct tune_queue_app_state *a);
int app_take_dirty(struct tune_queue_app_state *a);  // true once after a change

struct add_result app_add_urls(struct tune_queue_app_state *a,
                               const char **urls, int n);
void app_remove_from_queue(struct tune_queue_app_state *a,
                           const char *video_id);
void app_set_like(struct tune_queue_app_state *a, const char *video_id,
                  int liked);
int app_set_api_key(struct tune_queue_app_state *a, const char *key, char *err,
                    int errlen);

void app_play_video(struct tune_queue_app_state *a, const struct video *v,
                    double resume);
void app_play_queue_item(struct tune_queue_app_state *a,
                         const struct queue_item *it);
void app_play_now(struct tune_queue_app_state *a);
void app_press_play(struct tune_queue_app_state *a);
void app_advance(struct tune_queue_app_state *a, const char *outcome);

enum tune_queue_app_event app_poll(struct tune_queue_app_state *a, double dt);

#endif
