#include "frames.h"
#include "helper.h"
#include "youtube.h"

#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "stb_image.h"

extern char **environ;

static char outdir[1024];
static char framefile[1100];
static char mpv_bin[512];
static char ytdl_bin[512];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int running;

static char req_id[16];
static double req_pos;
static int has_req;
static unsigned char *ready_bytes;  // finished JPEG awaiting GL upload
static int ready_n;
static char ready_id[16];
static int has_ready;

static Texture2D tex;  // main-thread only
static int has_tex;
static char tex_id[16];

// run mpv to dump one software-decoded frame at pos into framefile
static void grab(const char *video_id, double pos)
{
    char url[YT_WATCH_URL_MAX];
    yt_watch_url(video_id, url);
    unlink(framefile);

    char start[32], outarg[1100], sopt[600];
    snprintf(start, sizeof(start), "--start=%d", pos > 0 ? (int)pos : 0);
    snprintf(outarg, sizeof(outarg), "--vo-image-outdir=%s", outdir);
    snprintf(sopt, sizeof(sopt), "--script-opts=ytdl_hook-ytdl_path=%s",
             ytdl_bin);
    char *argv[] = {
        mpv_bin,
        url,
        "--no-audio",
        "--no-terminal",
        "--really-quiet",
        start,
        "--frames=1",
        "--vo=image",
        "--vo-image-format=jpg",
        outarg,
        "--ytdl-format=best[height<=?720][vcodec!=none][protocol=https]/best",
        sopt,
        NULL
    };
    pid_t pid;
    if (posix_spawnp(&pid, mpv_bin, NULL, NULL, argv, environ) != 0) {
        return;
    }
    waitpid(pid, NULL, 0);

    int n = 0;
    unsigned char *buf = read_file(framefile, &n);
    if (!buf) {
        return;
    }
    pthread_mutex_lock(&lock);
    free(ready_bytes);
    ready_bytes = buf;
    ready_n = n;
    snprintf(ready_id, sizeof(ready_id), "%s", video_id);
    has_ready = 1;
    pthread_mutex_unlock(&lock);
}

static void *worker_main(void *arg)
{
    (void)arg;
    while (1) {
        pthread_mutex_lock(&lock);
        while (running && !has_req) {
            pthread_cond_wait(&cv, &lock);
        }
        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }
        char id[16];
        double pos = req_pos;
        snprintf(id, sizeof(id), "%s", req_id);
        has_req = 0;
        pthread_mutex_unlock(&lock);
        grab(id, pos);
    }
    return NULL;
}

void frames_init(const char *cache_dir, const char *mpv_path,
                 const char *ytdl_path)
{
    snprintf(outdir, sizeof(outdir), "%s/frames", cache_dir);
    mkdir_p(outdir);
    snprintf(framefile, sizeof(framefile), "%s/00000001.jpg", outdir);
    snprintf(mpv_bin, sizeof(mpv_bin), "%s", mpv_path);
    snprintf(ytdl_bin, sizeof(ytdl_bin), "%s", ytdl_path);
    running = 1;
    pthread_create(&worker, NULL, worker_main, NULL);
}

void frames_shutdown(void)
{
    pthread_mutex_lock(&lock);
    running = 0;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    free(ready_bytes);
    ready_bytes = NULL;
    if (has_tex) {
        UnloadTexture(tex);
    }
    has_tex = 0;
}

void frames_request(const char *video_id, double position)
{
    pthread_mutex_lock(&lock);
    snprintf(req_id, sizeof(req_id), "%s", video_id);
    req_pos = position;
    has_req = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
}

Texture2D *frames_get(const char *video_id)
{
    if (has_tex && strcmp(tex_id, video_id) == 0) {
        return &tex;
    }
    return NULL;
}

void frames_pump(void)
{
    pthread_mutex_lock(&lock);
    unsigned char *bytes = has_ready ? ready_bytes : NULL;
    int n = ready_n;
    char id[16];
    snprintf(id, sizeof(id), "%s", ready_id);
    if (has_ready) {
        ready_bytes = NULL;
        has_ready = 0;
    }
    pthread_mutex_unlock(&lock);
    if (!bytes) {
        return;
    }

    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(bytes, n, &w, &h, &ch, 4);
    free(bytes);
    if (!px) {
        return;
    }
    Image img = { px, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D nt = LoadTextureFromImage(img);
    SetTextureFilter(nt, TEXTURE_FILTER_BILINEAR);
    stbi_image_free(px);
    if (has_tex) {
        UnloadTexture(tex);
    }
    tex = nt;
    has_tex = 1;
    snprintf(tex_id, sizeof(tex_id), "%s", id);
}
