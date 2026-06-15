#include "youtube.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "cJSON.h"

#define API_URL "https://www.googleapis.com/youtube/v3/videos"
#define OEMBED_URL "https://www.youtube.com/oembed"

static const char *THUMB_PREFERENCE[] = { "maxres", "standard", "high",
                                          "medium", "default" };

void yt_watch_url(const char *id, char *out)
{
    snprintf(out, YT_WATCH_URL_MAX, "https://www.youtube.com/watch?v=%s", id);
}

static int is_video_id(const char *s, int len)
{
    if (len != 11) {
        return 0;
    }
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
            return 0;
        }
    }
    return 1;
}

// find query value for key within a "k=v&k2=v2" string; copies into out
static int query_value(const char *query, const char *key, char *out,
                       int outlen)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *seg_end = amp ? amp : p + strlen(p);
        if ((size_t)(seg_end - p) > klen && strncmp(p, key, klen) == 0 &&
            p[klen] == '=') {
            const char *v = p + klen + 1;
            int n = (int)(seg_end - v);
            if (n >= outlen) {
                n = outlen - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

int yt_extract_video_id(const char *raw, char *out)
{
    if (!raw) {
        return 0;
    }
    while (*raw && isspace((unsigned char)*raw)) {
        raw++;
    }
    int len = (int)strlen(raw);
    while (len > 0 && isspace((unsigned char)raw[len - 1])) {
        len--;
    }
    char buf[2048];
    if (len >= (int)sizeof(buf)) {
        return 0;
    }
    memcpy(buf, raw, len);
    buf[len] = '\0';

    if (is_video_id(buf, len)) {
        memcpy(out, buf, len + 1);
        return 1;
    }

    const char *sep = strstr(buf, "://");
    if (!sep) {
        return 0;
    }
    const char *host = sep + 3;
    const char *host_end = host;
    while (*host_end && *host_end != '/' && *host_end != '?' &&
           *host_end != '#') {
        host_end++;
    }
    char hostbuf[256];
    int hlen = (int)(host_end - host);
    if (hlen <= 0 || hlen >= (int)sizeof(hostbuf)) {
        return 0;
    }
    memcpy(hostbuf, host, hlen);
    hostbuf[hlen] = '\0';
    const char *h = hostbuf;
    if (strncmp(h, "www.", 4) == 0) {
        h += 4;
    }
    if (strncmp(h, "m.", 2) == 0) {
        h += 2;
    }

    const char *path = host_end;
    const char *query = strchr(path, '?');
    char pathbuf[1024];
    int plen = (int)((query ? query : path + strlen(path)) - path);
    if (plen < 0 || plen >= (int)sizeof(pathbuf)) {
        return 0;
    }
    memcpy(pathbuf, path, plen);
    pathbuf[plen] = '\0';
    const char *q = query ? query + 1 : "";

    char cand[64] = { 0 };
    if (strcmp(h, "youtu.be") == 0) {
        const char *seg = pathbuf;
        while (*seg == '/') {
            seg++;
        }
        const char *seg_end = seg;
        while (*seg_end && *seg_end != '/') {
            seg_end++;
        }
        int n = (int)(seg_end - seg);
        if (n > 0 && n < (int)sizeof(cand)) {
            memcpy(cand, seg, n);
            cand[n] = '\0';
        }
    } else if (strcmp(h, "youtube.com") == 0 ||
               strcmp(h, "music.youtube.com") == 0) {
        if (strcmp(pathbuf, "/watch") == 0) {
            query_value(q, "v", cand, sizeof(cand));
        } else if (strncmp(pathbuf, "/shorts/", 8) == 0 ||
                   strncmp(pathbuf, "/embed/", 7) == 0 ||
                   strncmp(pathbuf, "/live/", 6) == 0) {
            // third path segment ("", seg1, seg2)
            const char *seg = pathbuf + 1;
            seg = strchr(seg, '/');
            if (seg) {
                seg++;
                const char *seg_end = seg;
                while (*seg_end && *seg_end != '/') {
                    seg_end++;
                }
                int n = (int)(seg_end - seg);
                if (n > 0 && n < (int)sizeof(cand)) {
                    memcpy(cand, seg, n);
                    cand[n] = '\0';
                }
            }
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    if (is_video_id(cand, (int)strlen(cand))) {
        strcpy(out, cand);
        return 1;
    }
    return 0;
}

long yt_parse_iso8601_duration(const char *s)
{
    if (!s || *s != 'P') {
        return -1;
    }
    const char *p = s + 1;
    long days = 0, hh = 0, mm = 0, ss = 0;
    while (*p) {
        if (*p == 'T') {
            p++;
            continue;
        }
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        long n = 0;
        while (isdigit((unsigned char)*p)) {
            n = n * 10 + (*p - '0');
            p++;
        }
        char u = *p;
        if (!u) {
            return -1;
        }
        p++;
        switch (u) {
            case 'D':
                days = n;
                break;
            case 'H':
                hh = n;
                break;
            case 'M':
                mm = n;
                break;
            case 'S':
                ss = n;
                break;
            default:
                return -1;
        }
    }
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

char *yt_get_api_key_env(void)
{
    const char *env = getenv("YOUTUBE_API_KEY");
    if (env && *env) {
        // trim
        while (*env && isspace((unsigned char)*env)) {
            env++;
        }
        int n = (int)strlen(env);
        while (n > 0 && isspace((unsigned char)env[n - 1])) {
            n--;
        }
        if (n > 0) {
            char *k = malloc(n + 1);
            memcpy(k, env, n);
            k[n] = '\0';
            return k;
        }
    }
    const char *candidates[2];
    int nc = 0;
    const char *kf = getenv("YTQ_KEY_FILE");
    if (kf && *kf) {
        candidates[nc++] = kf;
    }
    candidates[nc++] = "data_v3_api_key.txt";
    for (int i = 0; i < nc; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) {
            continue;
        }
        char text[512];
        size_t r = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[r] = '\0';
        char *p = text;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        int n = (int)strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) {
            n--;
        }
        if (n > 0) {
            char *k = malloc(n + 1);
            memcpy(k, p, n);
            k[n] = '\0';
            return k;
        }
    }
    return NULL;
}

// --- HTTP ---
struct membuf {
    char *data;
    size_t len;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp)
{
    struct membuf *m = userp;
    size_t add = size * nmemb;
    char *grown = realloc(m->data, m->len + add + 1);
    if (!grown) {
        return 0;
    }
    m->data = grown;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

/*
 * GET url, return malloc'd body (caller frees) or NULL; *status set to HTTP
 * code.
 */
static char *http_get(const char *url, long *status)
{
    CURL *c = curl_easy_init();
    if (!c) {
        return NULL;
    }
    struct membuf m = { malloc(1), 0 };
    m.data[0] = '\0';
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "TuneQueue/1.0");
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (status) {
        *status = code;
    }
    if (rc != CURLE_OK) {
        free(m.data);
        return NULL;
    }
    return m.data;
}

static char *esc(const char *s)
{
    char *e = curl_easy_escape(NULL, s, 0);
    char *out = e ? strdup(e) : strdup("");
    if (e) {
        curl_free(e);
    }
    return out;
}

static struct str json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring && *v->valuestring) {
        return str_from(v->valuestring);
    }
    return str_from(NULL);
}

static struct str best_thumbnail(const cJSON *thumbs)
{
    if (!cJSON_IsObject(thumbs)) {
        return str_from(NULL);
    }
    for (size_t i = 0; i < sizeof(THUMB_PREFERENCE) / sizeof(*THUMB_PREFERENCE);
         i++) {
        const cJSON *t =
            cJSON_GetObjectItemCaseSensitive(thumbs, THUMB_PREFERENCE[i]);
        struct str u = json_str(t, "url");
        if (u.len) {
            return u;
        }
        str_free(&u);
    }
    return str_from(NULL);
}

int yt_validate_key(const char *key, char *err, int errlen)
{
    char *ek = esc(key);
    struct str url = str_from(NULL);
    str_appendf(&url, "%s?part=id&id=dQw4w9WgXcQ&key=%s", API_URL, ek);
    free(ek);
    long status = 0;
    char *body = http_get(str_c(&url), &status);
    str_free(&url);
    if (!body) {
        snprintf(err, errlen, "could not reach YouTube");
        return 0;
    }
    if (status >= 400) {
        snprintf(err, errlen, "key rejected by YouTube (HTTP %ld)", status);
        free(body);
        return 0;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    int ok = 1;
    if (j && cJSON_GetObjectItemCaseSensitive(j, "error")) {
        snprintf(err, errlen, "key rejected by YouTube");
        ok = 0;
    }
    cJSON_Delete(j);
    return ok;
}

static void meta_from_oembed(const char *id, struct video_meta *out)
{
    out->ok = 0;
    char w[YT_WATCH_URL_MAX];
    yt_watch_url(id, w);
    char *ew = esc(w);
    struct str url = str_from(NULL);
    str_appendf(&url, "%s?url=%s&format=json", OEMBED_URL, ew);
    free(ew);
    long status = 0;
    char *body = http_get(str_c(&url), &status);
    str_free(&url);
    if (!body || status >= 400) {
        free(body);
        return;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return;
    }
    strncpy(out->id, id, sizeof(out->id) - 1);
    out->title = json_str(j, "title");
    out->channel_title = json_str(j, "author_name");
    out->description = str_from(NULL);
    out->thumbnail_url = json_str(j, "thumbnail_url");
    out->duration_seconds = -1;
    strcpy(out->source, "oembed");
    out->ok = 1;
    cJSON_Delete(j);
}

static void fetch_via_api(const char **ids, int n, const char *key,
                          struct video_meta *out)
{
    char *ek = esc(key);
    for (int start = 0; start < n; start += 50) {
        int end = start + 50 < n ? start + 50 : n;
        // join ids with comma
        char joined[50 * 13];
        joined[0] = '\0';
        for (int i = start; i < end; i++) {
            if (i > start) {
                strcat(joined, ",");
            }
            strcat(joined, ids[i]);
        }
        struct str url = str_from(NULL);
        str_appendf(&url, "%s?part=snippet,contentDetails&id=%s&key=%s",
                    API_URL, joined, ek);
        long status = 0;
        char *body = http_get(str_c(&url), &status);
        str_free(&url);
        if (!body) {
            continue;
        }
        cJSON *j = cJSON_Parse(body);
        free(body);
        if (!j) {
            continue;
        }
        cJSON *items = cJSON_GetObjectItemCaseSensitive(j, "items");
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items)
        {
            const cJSON *idv = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!cJSON_IsString(idv)) {
                continue;
            }
            // find matching out slot
            struct video_meta *slot = NULL;
            for (int i = start; i < end; i++) {
                if (strcmp(ids[i], idv->valuestring) == 0) {
                    slot = &out[i];
                    break;
                }
            }
            if (!slot) {
                continue;
            }
            const cJSON *snippet =
                cJSON_GetObjectItemCaseSensitive(item, "snippet");
            const cJSON *cd =
                cJSON_GetObjectItemCaseSensitive(item, "contentDetails");
            const cJSON *durv =
                cd ? cJSON_GetObjectItemCaseSensitive(cd, "duration") : NULL;
            strncpy(slot->id, idv->valuestring, sizeof(slot->id) - 1);
            slot->title = json_str(snippet, "title");
            slot->channel_title = json_str(snippet, "channelTitle");
            slot->description = json_str(snippet, "description");
            slot->thumbnail_url =
                best_thumbnail(snippet ? cJSON_GetObjectItemCaseSensitive(
                                             snippet, "thumbnails")
                                       : NULL);
            slot->duration_seconds =
                (cJSON_IsString(durv))
                    ? yt_parse_iso8601_duration(durv->valuestring)
                    : -1;
            strcpy(slot->source, "api");
            slot->ok = 1;
        }
        cJSON_Delete(j);
    }
    free(ek);
}

void yt_fetch_metadata(const char **ids, int n, const char *api_key,
                       struct video_meta *out)
{
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].duration_seconds = -1;
    }
    if (api_key && *api_key) {
        fetch_via_api(ids, n, api_key, out);
    } else {
        for (int i = 0; i < n; i++) {
            meta_from_oembed(ids[i], &out[i]);
        }
    }
}

void yt_meta_free(struct video_meta *m)
{
    str_free(&m->title);
    str_free(&m->channel_title);
    str_free(&m->description);
    str_free(&m->thumbnail_url);
}
