// Core state machine — drives the app_* interface directly (no frontend) and
// checks the now-playing lifecycle. It opens its own throwaway database, seeds
// one row, and advancing skips it, so it never touches the real queue.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "tmpdb.h"

#define SEED_ID "tqStateTest"

static int fails;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        printf("  [%s] %s\n", (cond) ? "ok" : "FAIL", msg); \
        if (!(cond)) fails++;                               \
    } while (0)

int main(void)
{
    printf("== core state ==\n");
    char dbp[512];
    tmpdb_path(dbp, sizeof(dbp));
    unlink(dbp);
    // seed one queue item so the transitions run deterministically, offline
    sqlite3 *seed = db_open(dbp);
    sqlite3_exec(
        seed,
        "INSERT OR IGNORE INTO videos(id,url,metadata_source) VALUES('" SEED_ID
        "','https://youtu.be/" SEED_ID
        "','none');"
        "INSERT OR IGNORE INTO queue_items(video_id,position) VALUES('" SEED_ID
        "',1);",
        0, 0, 0);
    sqlite3_close(seed);

    struct tune_queue_app_state a;
    struct tune_queue_app_config cfg = { .core = { .db_path = dbp } };
    app_init(&a, &cfg);
    CHECK(a.db != NULL, "app_init opens the database");

    if (a.queue.count == 0) {
        puts("  [FAIL] seed did not load");
        app_shutdown(&a);
        unlink(dbp);
        return 1;
    }

    char first[16];
    snprintf(first, sizeof(first), "%s", a.queue.items[0].video.id);
    CHECK(a.has_now && strcmp(a.now.id, first) == 0,
          "first queue item is cued without a session");
    CHECK(a.session_id == 0, "a cued track has no open session");

    app_play_queue_item(&a, &a.queue.items[0]);
    CHECK(a.has_now && strcmp(a.now.id, first) == 0,
          "play sets the now-playing track");
    CHECK(a.session_id > 0, "play opens a recorded session");
    CHECK(a.player.status == PS_LOADING, "play hands the track to the player");

    long long sid = a.session_id;
    app_advance(&a, "skipped");
    // advance closes this session, then opens one for the next track (or goes
    // idle if the queue is empty); either way the old session is no longer
    // current
    CHECK(a.session_id != sid, "advance ends the finished session");

    app_shutdown(&a);
    unlink(dbp);
    printf("%s (%d failures)\n", fails ? "FAILURES" : "all ok", fails);
    return fails ? 1 : 0;
}
