#ifndef YOUTUBE_H
#define YOUTUBE_H

#include "str.h"

#define YT_WATCH_URL_MAX 64

struct video_meta {
    char id[16];
    struct str title;
    struct str channel_title;
    struct str description;
    struct str thumbnail_url;
    long duration_seconds;  // -1 when unknown
    char source[12];        // "api" | "oembed"
    int ok;                 // 1 if metadata was found
};

// extract an 11-char video id from a URL or bare id into out (>=16); 1 on
// success
int yt_extract_video_id(const char *url, char *out);

// build a canonical watch URL into out (>=YT_WATCH_URL_MAX bytes)
void yt_watch_url(const char *id, char *out);

// parse an ISO-8601 duration (e.g. "PT1H2M3S") to seconds; -1 on failure
long yt_parse_iso8601_duration(const char *s);

// resolve the API key from env or data_v3_api_key.txt; malloc'd or NULL
char *yt_get_api_key_env(void);

// shape-check a Data API key; 1 if plausible, else 0 with a message in err
int yt_validate_key(const char *key, char *err, int errlen);

/*
 * Fetch metadata for n ids into out (n entries). Uses the Data API when
 * api_key is non-NULL, otherwise oEmbed per-video. Missing entries have ok=0.
 */
void yt_fetch_metadata(const char **ids, int n, const char *api_key,
                       struct video_meta *out);

void yt_meta_free(struct video_meta *m);

#endif
