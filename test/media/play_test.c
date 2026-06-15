// Integration: resolve and play a video through the app's player (libmpv +
// yt-dlp), the same path the running app uses. Reports the typed failure.
//
//   test-play            play the first queued video (or a known-good fallback)
//   test-play <id>       play a specific video id
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "player.h"
#include "tmpdb.h"

static const char *fail_name(enum player_fail f)
{
    switch (f) {
        case PF_TIMEOUT:
            return "TIMEOUT";
        case PF_UNRESOLVED:
            return "UNRESOLVED";
        case PF_PLAYBACK:
            return "PLAYBACK";
        default:
            return "NONE";
    }
}

static int play(const char *id)
{
    printf("== playback: %s ==\n", id);
    struct player p;
    player_init(&p, "yt-dlp");
    player_load(&p, id, 0);
    time_t start = time(NULL);
    for (;;) {
        player_poll(&p);
        if (player_take_failed(&p)) {
            printf("  [FAIL] %s: %s\n", fail_name(p.fail),
                   p.fail_msg[0] ? p.fail_msg : "(no detail)");
            player_destroy(&p);
            return 1;
        }
        if (p.status == PS_PLAYING && p.position > 0.5) {
            printf("  [ok] playing at %.1fs (%lds to start)\n", p.position,
                   (long)(time(NULL) - start));
            player_stop(&p);
            player_destroy(&p);
            return 0;
        }
        if (time(NULL) - start >= 50) {
            printf("  [FAIL] no playback within 50s (status=%d)\n", p.status);
            player_destroy(&p);
            return 1;
        }
        usleep(200 * 1000);
    }
}

int main(int argc, char **argv)
{
    char id[16];
    if (argc > 1) {
        snprintf(id, sizeof(id), "%s", argv[1]);
    } else {
        char dbp[512];
        tmpdb_path(dbp, sizeof(dbp));
        unlink(dbp);
        sqlite3 *db = db_open(dbp);  // throwaway: never the real queue
        struct queue_list q = db_list_queue(db);
        snprintf(id, sizeof(id), "%s",
                 q.count > 0 ? q.items[0].video.id : "dQw4w9WgXcQ");
        queue_list_free(&q);
        sqlite3_close(db);
        unlink(dbp);
    }
    return play(id);
}
