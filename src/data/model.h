#ifndef MODEL_H
#define MODEL_H

#include "str.h"

// shared data structs; empty text has len 0, duration_seconds is -1 if unknown

struct video {
    char id[16];
    struct str url;
    struct str title;
    struct str channel_title;
    struct str description;
    struct str thumbnail_url;
    long duration_seconds;
    char metadata_source[12];
};

struct queue_item {
    long long queue_id;
    char *added_at;
    int liked;
    int has_resume;
    double resume_position;
    struct video video;
};

struct history_entry {
    long long session_id;
    char *started_at;
    char *ended_at;
    double seconds_listened;
    double max_position;
    char *outcome;
    int liked;
    int queued;
    struct video video;
};

struct liked_entry {
    char *liked_at;
    struct video video;
};

struct day_stat {
    char date[16];
    double seconds;
};

struct stats {
    double total_seconds;
    long total_videos;
    long completed_count;
    long skipped_count;
    struct day_stat *per_day;
    int per_day_count;
};

void video_free(struct video *v);

#endif
