// Integration: drive the shared app command layer the way a frontend does.
// The test is its own frontend: it builds a config with a throwaway database,
// brings up the app, and runs a sequence of commands, so it never touches the
// real queue.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "tmpdb.h"

static int fails;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        printf("  [%s] %s\n", (cond) ? "ok" : "FAIL", msg); \
        if (!(cond)) fails++;                               \
    } while (0)

int main(void)
{
    printf("== cli ==\n");
    char dbp[512];
    tmpdb_path(dbp, sizeof(dbp));
    unlink(dbp);

    sqlite3 *seed = db_open(dbp);
    sqlite3_exec(seed,
                 "INSERT INTO videos(id,url,metadata_source) VALUES"
                 "('tqCliA','https://youtu.be/tqCliA','none'),"
                 "('tqCliB','https://youtu.be/tqCliB','none');"
                 "INSERT INTO queue_items(video_id,position) VALUES"
                 "('tqCliA',1),('tqCliB',2);",
                 0, 0, 0);
    sqlite3_close(seed);

    struct tune_queue_app_state a;
    struct tune_queue_app_config cfg = {
        .core = { .db_path = dbp,
                  .ytdl_path = "yt-dlp",
                  .heartbeat_interval = TUNE_QUEUE_DEFAULT_HEARTBEAT_INTERVAL }
    };
    app_init(&a, &cfg);

    CHECK(a.queue.count == 2, "the queue loads from the database");
    CHECK(a.has_now && strcmp(a.now.id, "tqCliA") == 0,
          "the first track is cued");

    app_set_like(&a, "tqCliA", 1);
    app_reload(&a);
    CHECK(a.liked.count == 1, "like adds the track to the liked list");

    app_play_queue_item(&a, &a.queue.items[0]);
    CHECK(a.session_id > 0, "play opens a session");
    app_advance(&a, "skipped");
    CHECK(strcmp(a.now.id, "tqCliB") == 0, "advance moves to the next track");

    app_shutdown(&a);
    unlink(dbp);
    printf("%s (%d failures)\n", fails ? "FAILURES" : "all ok", fails);
    return fails ? 1 : 0;
}
