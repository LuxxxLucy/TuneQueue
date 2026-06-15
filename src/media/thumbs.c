#include "thumbs.h"
#include "helper.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "stb_image.h"

enum { ST_LOADING, ST_BYTES, ST_READY, ST_FAILED };

#define MAX_THUMBS 512

struct entry {
    char key[16];
    char *url;
    int state;
    unsigned char *bytes;
    int nbytes;
    Texture2D tex;
    int has_tex;
};

static struct entry entries[MAX_THUMBS];
static int entry_count = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int running = 0;

struct membuf {
    unsigned char *data;
    size_t len;
};

static size_t wr(void *ptr, size_t s, size_t n, void *u)
{
    struct membuf *m = u;
    size_t add = s * n;
    unsigned char *g = realloc(m->data, m->len + add);
    if (!g) {
        return 0;
    }
    m->data = g;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    return add;
}

static char thumb_dir[1024];

static void thumb_path(const char *key, char *out, int n)
{
    snprintf(out, n, "%s/%s", thumb_dir, key);
}

static int find_pending(void)
{
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].state == ST_LOADING && entries[i].url) {
            return i;
        }
    }
    return -1;
}

static void *worker_main(void *arg)
{
    (void)arg;
    CURL *c = curl_easy_init();
    while (1) {
        pthread_mutex_lock(&lock);
        int idx;
        while (running && (idx = find_pending()) < 0) {
            pthread_cond_wait(&cv, &lock);
        }
        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }
        char url[1024], key[16];
        snprintf(url, sizeof(url), "%s", entries[idx].url);
        snprintf(key, sizeof(key), "%s", entries[idx].key);
        // mark in-flight by clearing url so we don't pick it again
        free(entries[idx].url);
        entries[idx].url = NULL;
        pthread_mutex_unlock(&lock);

        char path[1100];
        thumb_path(key, path, sizeof(path));

        // serve from the on-disk cache when present
        int dlen = 0;
        unsigned char *disk = read_file(path, &dlen);
        if (disk) {
            pthread_mutex_lock(&lock);
            entries[idx].bytes = disk;
            entries[idx].nbytes = dlen;
            entries[idx].state = ST_BYTES;
            pthread_mutex_unlock(&lock);
            continue;
        }

        struct membuf m = { NULL, 0 };
        curl_easy_reset(c);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        CURLcode rc = curl_easy_perform(c);

        pthread_mutex_lock(&lock);
        if (rc == CURLE_OK && m.len > 0) {
            FILE *w = fopen(path, "wb");
            if (w) {
                fwrite(m.data, 1, m.len, w);
                fclose(w);
            }
            entries[idx].bytes = m.data;
            entries[idx].nbytes = (int)m.len;
            entries[idx].state = ST_BYTES;
        } else {
            free(m.data);
            entries[idx].state = ST_FAILED;
        }
        pthread_mutex_unlock(&lock);
    }
    if (c) {
        curl_easy_cleanup(c);
    }
    return NULL;
}

void thumbs_init(const char *cache_dir)
{
    snprintf(thumb_dir, sizeof(thumb_dir), "%s/thumbnails", cache_dir);
    mkdir_p(thumb_dir);
    running = 1;
    pthread_create(&worker, NULL, worker_main, NULL);
}

void thumbs_shutdown(void)
{
    pthread_mutex_lock(&lock);
    running = 0;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    for (int i = 0; i < entry_count; i++) {
        free(entries[i].url);
        free(entries[i].bytes);
        if (entries[i].has_tex) {
            UnloadTexture(entries[i].tex);
        }
    }
    entry_count = 0;
}

Texture2D *thumbs_get(const char *key, const char *url)
{
    if (!key || !key[0] || !url || !url[0]) {
        return NULL;
    }
    Texture2D *result = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].key, key) == 0) {
            if (entries[i].state == ST_READY) {
                result = &entries[i].tex;
            }
            pthread_mutex_unlock(&lock);
            return result;
        }
    }
    if (entry_count < MAX_THUMBS) {
        struct entry *e = &entries[entry_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->key, key, sizeof(e->key) - 1);
        e->url = strdup(url);
        e->state = ST_LOADING;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void thumbs_pump(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].state != ST_BYTES) {
            continue;
        }
        unsigned char *bytes = entries[i].bytes;
        int n = entries[i].nbytes;
        entries[i].bytes = NULL;
        pthread_mutex_unlock(&lock);

        int w, h, ch;
        unsigned char *px = stbi_load_from_memory(bytes, n, &w, &h, &ch, 4);
        free(bytes);
        Texture2D tex = { 0 };
        int ok = 0;
        if (px) {
            Image img = { px, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            tex = LoadTextureFromImage(img);
            SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
            stbi_image_free(px);
            ok = 1;
        }
        pthread_mutex_lock(&lock);
        if (ok) {
            entries[i].tex = tex;
            entries[i].has_tex = 1;
            entries[i].state = ST_READY;
        } else {
            entries[i].state = ST_FAILED;
        }
    }
    pthread_mutex_unlock(&lock);
}
