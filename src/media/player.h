#ifndef PLAYER_H
#define PLAYER_H

enum player_status { PS_IDLE, PS_LOADING, PS_PLAYING, PS_PAUSED };

// why a load failed, so the UI can pick the right response
enum player_fail {
    PF_NONE,
    PF_TIMEOUT,     // stalled past the load watchdog
    PF_UNRESOLVED,  // never opened the stream (YouTube block, removed video)
    PF_PLAYBACK,    // stopped with an error after it started
};

typedef struct mpv_handle mpv_handle;

struct player {
    mpv_handle *mpv;
    enum player_status status;
    char video_id[16];
    int has_current;
    double position;        // seconds
    double duration;        // seconds, 0 if unknown
    double volume;          // 0..100
    int ended;              // set once on natural end-of-file
    int failed;             // set when a load aborts or stalls out
    enum player_fail fail;  // classification of the last failure
    char fail_msg[160];     // mpv reason string, for logs and the test
    long load_started;      // epoch seconds the current load began
};

// ytdl_path is the yt-dlp binary mpv should call, or NULL to search PATH
void player_init(struct player *p, const char *ytdl_path);
void player_load(struct player *p, const char *video_id,
                 double resume_position);
void player_toggle(struct player *p);
void player_set_paused(struct player *p, int paused);
void player_seek(struct player *p, double pos);
void player_set_volume(struct player *p, double vol);
void player_poll(struct player *p);
int player_take_ended(struct player *p);   // 1 exactly once when track ends
int player_take_failed(struct player *p);  // 1 exactly once when a load fails
void player_stop(struct player *p);

void player_destroy(struct player *p);

#endif
