#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helper.h"

static void video_copy(struct video *dst, const struct video *src)
{
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->id, src->id, sizeof(dst->id));
    memcpy(dst->metadata_source, src->metadata_source,
           sizeof(dst->metadata_source));
    dst->duration_seconds = src->duration_seconds;
    dst->url = str_from(str_c(&src->url));
    dst->title = str_from(str_c(&src->title));
    dst->channel_title = str_from(str_c(&src->channel_title));
    dst->description = str_from(str_c(&src->description));
    dst->thumbnail_url = str_from(str_c(&src->thumbnail_url));
}

void app_reload(struct tune_queue_app_state *a)
{
    queue_list_free(&a->queue);
    history_list_free(&a->history);
    liked_list_free(&a->liked);
    stats_free(&a->stats);
    a->queue = db_list_queue(a->db);
    a->history = db_list_history(a->db, 100, 0);
    a->liked = db_list_likes(a->db);
    a->stats = db_stats(a->db);
    a->dirty = 0;
}

int app_take_dirty(struct tune_queue_app_state *a)
{
    if (a->dirty) {
        a->dirty = 0;
        return 1;
    }
    return 0;
}

void app_init(struct tune_queue_app_state *a,
              const struct tune_queue_app_config *cfg)
{
    memset(a, 0, sizeof(*a));
    a->heartbeat_interval = cfg->core.heartbeat_interval;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", cfg->core.db_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir);
    }
    a->db = db_open(cfg->core.db_path);
    player_init(&a->player, cfg->core.ytdl_path);
    app_reload(a);
    if (a->queue.count > 0) {  // cue the first track without starting it
        video_copy(&a->now, &a->queue.items[0].video);
        a->has_now = 1;
    }
}

void app_shutdown(struct tune_queue_app_state *a)
{
    if (a->session_id > 0) {
        db_abandon_open_sessions(a->db, NULL);
    }
    player_destroy(&a->player);
    queue_list_free(&a->queue);
    history_list_free(&a->history);
    liked_list_free(&a->liked);
    stats_free(&a->stats);
    video_free(&a->now);
    if (a->db) {
        sqlite3_close(a->db);
    }
}

struct add_result app_add_urls(struct tune_queue_app_state *a,
                               const char **urls, int n)
{
    struct add_result r = db_add_to_queue(a->db, urls, n);
    if (r.added > 0) {
        a->dirty = 1;
    }
    return r;
}

void app_set_like(struct tune_queue_app_state *a, const char *video_id,
                  int liked)
{
    int rc =
        liked ? db_add_like(a->db, video_id) : db_remove_like(a->db, video_id);
    if (rc == 0) {
        a->dirty = 1;
    }
}

int app_set_api_key(struct tune_queue_app_state *a, const char *key, char *err,
                    int errlen)
{
    int rc = db_put_api_key(a->db, key, err, errlen);
    if (rc == 0) {
        a->dirty = 1;
    }
    return rc;
}

// close the recorded session for the current track, optionally handing back the
// next queued item
static void finish_current(struct tune_queue_app_state *a, const char *outcome,
                           struct queue_item *next, int *has_next)
{
    *has_next = 0;
    if (a->session_id > 0) {
        struct queue_item nx;
        int hn = db_finish_session(a->db, a->session_id, outcome, a->watched,
                                   a->player.position, &nx);
        a->session_id = 0;
        if (next && hn) {
            *next = nx;
            *has_next = 1;
        } else if (hn) {
            video_free(&nx.video);
            free(nx.added_at);
        }
        a->dirty = 1;
    }
}

void app_play_video(struct tune_queue_app_state *a, const struct video *v,
                    double resume)
{
    video_free(&a->now);
    video_copy(&a->now, v);
    a->has_now = 1;
    a->watched = 0;
    a->hb_accum = 0;
    a->session_id = db_start_session(a->db, v->id);
    player_load(&a->player, v->id, resume);
    a->dirty = 1;
}

void app_play_queue_item(struct tune_queue_app_state *a,
                         const struct queue_item *it)
{
    app_play_video(a, &it->video, it->has_resume ? it->resume_position : 0);
}

// play the current track, resuming from its queue position if still cued there
void app_play_now(struct tune_queue_app_state *a)
{
    if (!a->has_now) {
        return;
    }
    for (int i = 0; i < a->queue.count; i++) {
        if (strcmp(a->queue.items[i].video.id, a->now.id) == 0) {
            app_play_queue_item(a, &a->queue.items[i]);
            return;
        }
    }
    struct video tmp;  // copy first: app_play_video frees a->now before copying
    video_copy(&tmp, &a->now);
    app_play_video(a, &tmp, 0);
    video_free(&tmp);
}

void app_advance(struct tune_queue_app_state *a, const char *outcome)
{
    struct queue_item next;
    int has_next;
    finish_current(a, outcome, &next, &has_next);
    if (has_next) {
        app_play_video(a, &next.video,
                       next.has_resume ? next.resume_position : 0);
        video_free(&next.video);
        free(next.added_at);
    } else {
        a->player.status = PS_IDLE;
    }
}

void app_press_play(struct tune_queue_app_state *a)
{
    if (!a->has_now && a->queue.count > 0) {
        app_play_queue_item(a, &a->queue.items[0]);
        return;
    }
    if (!a->has_now) {
        return;
    }
    if (a->session_id <= 0) {  // cued but not started: resume from the queue
        app_play_now(a);
        return;
    }
    player_toggle(&a->player);
}

enum tune_queue_app_event app_poll(struct tune_queue_app_state *a, double dt)
{
    player_poll(&a->player);

    if (a->player.status == PS_PLAYING) {
        a->watched += dt;
        a->hb_accum += dt;
        if (a->hb_accum >= a->heartbeat_interval) {
            db_heartbeat(a->db, a->session_id, a->watched, a->player.position,
                         a->player.duration);
            a->hb_accum = 0;
        }
    }

    if (player_take_ended(&a->player)) {
        app_advance(a, "completed");
        return TUNE_QUEUE_APP_NONE;
    }
    if (player_take_failed(&a->player)) {
        int hn;
        finish_current(a, "abandoned", NULL, &hn);
        player_stop(&a->player);
        return TUNE_QUEUE_APP_TRACK_FAILED;
    }
    return TUNE_QUEUE_APP_NONE;
}
