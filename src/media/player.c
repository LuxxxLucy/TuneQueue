#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mpv/client.h>

#include "youtube.h"

#define LOAD_TIMEOUT 30  // seconds before a stuck load is declared failed

static void set_opt(mpv_handle *m, const char *name, const char *val)
{
    mpv_set_option_string(m, name, val);
}

void player_init(struct player *p, const char *ytdl_path)
{
    memset(p, 0, sizeof(*p));
    p->status = PS_IDLE;
    p->volume = 100.0;
    p->mpv = mpv_create();
    if (!p->mpv) {
        fprintf(stderr, "mpv_create failed\n");
        return;
    }

    // audio only: mpv's macOS video path is Metal, and its OpenGL render API
    // composites black on the Metal-emulated GL that raylib uses, so we decode
    // audio and show the track's album art on the stage instead
    set_opt(p->mpv, "vo", "null");
    set_opt(p->mpv, "vid", "no");
    set_opt(p->mpv, "force-window", "no");
    set_opt(p->mpv, "idle", "yes");
    set_opt(p->mpv, "ytdl", "yes");
    set_opt(p->mpv, "ytdl-format", "bestaudio/best");
    // authenticate yt-dlp with the user's Chrome cookies; without a logged-in
    // session YouTube blocks resolution as a suspected bot
    set_opt(p->mpv, "ytdl-raw-options", "cookies-from-browser=chrome");
    set_opt(p->mpv, "cache", "yes");
    // point mpv at an explicit yt-dlp; a bare name or NULL lets mpv search PATH
    if (ytdl_path && strchr(ytdl_path, '/')) {
        char opt[600];
        snprintf(opt, sizeof(opt), "ytdl_hook-ytdl_path=%s", ytdl_path);
        set_opt(p->mpv, "script-opts", opt);
    }
    set_opt(p->mpv, "prefetch-playlist", "no");

    if (mpv_initialize(p->mpv) < 0) {
        fprintf(stderr, "mpv_initialize failed\n");
        return;
    }

    mpv_observe_property(p->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(p->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(p->mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_set_property_string(p->mpv, "volume", "100");
}

void player_load(struct player *p, const char *video_id, double resume_position)
{
    if (!p->mpv) {
        return;
    }
    strncpy(p->video_id, video_id, sizeof(p->video_id) - 1);
    p->video_id[sizeof(p->video_id) - 1] = '\0';
    p->has_current = 1;
    p->status = PS_LOADING;
    p->position = 0;
    p->duration = 0;
    p->ended = 0;
    p->failed = 0;
    p->fail = PF_NONE;
    p->fail_msg[0] = '\0';
    p->load_started = time(NULL);
    char url[YT_WATCH_URL_MAX];
    yt_watch_url(video_id, url);
    if (resume_position > 1.0) {
        // mpv 0.38+ loadfile is <url> <flags> <index> <options>: the seek
        // option goes in the 5th slot, with -1 for the unused insert index
        char start[32];
        snprintf(start, sizeof(start), "start=%d", (int)resume_position);
        const char *cmd[] = { "loadfile", url, "replace", "-1", start, NULL };
        mpv_command(p->mpv, cmd);
    } else {
        const char *cmd[] = { "loadfile", url, "replace", NULL };
        mpv_command(p->mpv, cmd);
    }
    mpv_set_property_string(p->mpv, "pause", "no");
}

void player_set_paused(struct player *p, int paused)
{
    if (!p->mpv || !p->has_current) {
        return;
    }
    mpv_set_property_string(p->mpv, "pause", paused ? "yes" : "no");
}

void player_toggle(struct player *p)
{
    if (!p->has_current) {
        return;
    }
    player_set_paused(p, p->status == PS_PLAYING);
}

void player_seek(struct player *p, double pos)
{
    if (!p->mpv || !p->has_current) {
        return;
    }
    char s[32];
    snprintf(s, sizeof(s), "%.3f", pos);
    const char *cmd[] = { "seek", s, "absolute", NULL };
    mpv_command(p->mpv, cmd);
    p->position = pos;
}

void player_set_volume(struct player *p, double vol)
{
    if (!p->mpv) {
        return;
    }
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    p->volume = vol;
    char s[16];
    snprintf(s, sizeof(s), "%.0f", vol);
    mpv_set_property_string(p->mpv, "volume", s);
}

void player_poll(struct player *p)
{
    if (!p->mpv) {
        return;
    }
    // a load that never reaches FILE_LOADED (wedged yt-dlp, dead network) fires
    // no mpv event, so treat an over-long PS_LOADING as a failure
    if (p->status == PS_LOADING &&
        time(NULL) - p->load_started >= LOAD_TIMEOUT) {
        p->failed = 1;
        p->fail = PF_TIMEOUT;
        snprintf(p->fail_msg, sizeof(p->fail_msg), "load timed out after %ds",
                 LOAD_TIMEOUT);
        p->status = PS_IDLE;
    }
    while (1) {
        mpv_event *ev = mpv_wait_event(p->mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) {
            break;
        }
        switch (ev->event_id) {
            case MPV_EVENT_END_FILE: {
                mpv_event_end_file *ef = ev->data;
                if (ef->reason == MPV_END_FILE_REASON_EOF) {
                    p->ended = 1;
                } else if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                    p->failed = 1;
                    // no real position means the stream never opened
                    p->fail = p->position > 0.5 ? PF_PLAYBACK : PF_UNRESOLVED;
                    snprintf(p->fail_msg, sizeof(p->fail_msg), "%s",
                             mpv_error_string(ef->error));
                } else {
                    break;
                }
                p->status = PS_IDLE;
                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                mpv_event_property *pr = ev->data;
                if (strcmp(pr->name, "time-pos") == 0 &&
                    pr->format == MPV_FORMAT_DOUBLE) {
                    p->position = *(double *)pr->data;
                } else if (strcmp(pr->name, "duration") == 0 &&
                           pr->format == MPV_FORMAT_DOUBLE) {
                    p->duration = *(double *)pr->data;
                } else if (strcmp(pr->name, "pause") == 0 &&
                           pr->format == MPV_FORMAT_FLAG) {
                    int paused = *(int *)pr->data;
                    if (p->has_current) {
                        p->status = paused ? PS_PAUSED : PS_PLAYING;
                    }
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                p->status = PS_PLAYING;
                break;
            default:
                break;
        }
    }
}

void player_stop(struct player *p)
{
    if (!p->mpv) {
        return;
    }
    const char *cmd[] = { "stop", NULL };
    mpv_command(p->mpv, cmd);
    p->status = PS_IDLE;
    p->has_current = 0;
}

int player_take_ended(struct player *p)
{
    if (p->ended) {
        p->ended = 0;
        return 1;
    }
    return 0;
}

int player_take_failed(struct player *p)
{
    if (p->failed) {
        p->failed = 0;
        return 1;
    }
    return 0;
}

void player_destroy(struct player *p)
{
    if (p->mpv) {
        mpv_terminate_destroy(p->mpv);
        p->mpv = NULL;
    }
}
