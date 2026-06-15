// CLI frontend for the TuneQueue.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include "app.h"
#include "db.h"
#include "helper.h"
#include "player.h"

static const char *status_name(enum player_status s)
{
    switch (s) {
        case PS_LOADING:
            return "loading";
        case PS_PLAYING:
            return "playing";
        case PS_PAUSED:
            return "paused";
        default:
            return "idle";
    }
}

static void fmt_time(double sec, char *out, int n)
{
    if (sec < 0) {
        sec = 0;
    }
    long t = (long)(sec + 0.5);
    snprintf(out, n, "%ld:%02ld", t / 60, t % 60);
}

static void print_row(int i, const struct video *v, int liked)
{
    char d[16];
    fmt_time(v->duration_seconds < 0 ? 0 : v->duration_seconds, d, sizeof(d));
    printf("  %2d. %-40.40s  %5s %s\n", i + 1,
           v->title.len ? str_c(&v->title) : v->id, d, liked ? "♥" : "");
}

static void cmd_queue(struct tune_queue_app_state *a)
{
    printf("Queue (%d)\n", a->queue.count);
    for (int i = 0; i < a->queue.count; i++) {
        print_row(i, &a->queue.items[i].video, a->queue.items[i].liked);
    }
}

static void cmd_liked(struct tune_queue_app_state *a)
{
    printf("Liked (%d)\n", a->liked.count);
    for (int i = 0; i < a->liked.count; i++) {
        print_row(i, &a->liked.items[i].video, 1);
    }
}

static void cmd_history(struct tune_queue_app_state *a)
{
    printf("History (%d)\n", a->history.count);
    for (int i = 0; i < a->history.count; i++) {
        print_row(i, &a->history.items[i].video, a->history.items[i].liked);
    }
}

static void cmd_now(struct tune_queue_app_state *a)
{
    if (!a->has_now) {
        puts("Nothing cued.");
        return;
    }
    char pos[16], dur[16];
    fmt_time(a->player.position, pos, sizeof(pos));
    fmt_time(a->player.duration, dur, sizeof(dur));
    printf("[%s] %s  %s/%s\n", status_name(a->player.status),
           a->now.title.len ? str_c(&a->now.title) : a->now.id, pos, dur);
}

struct command {
    const char *name;
    const char *args;
    const char *desc;
    int (*fn)(struct tune_queue_app_state *a, char **save);
};

static const struct command CMDS[];

static int do_queue(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    cmd_queue(a);
    return 0;
}

static int do_liked(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    cmd_liked(a);
    return 0;
}

static int do_history(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    cmd_history(a);
    return 0;
}

static int do_now(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    cmd_now(a);
    return 0;
}

static int do_reload(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    app_reload(a);
    puts("reloaded.");
    return 0;
}

static int do_pause(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    player_toggle(&a->player);
    return 0;
}

static int do_stop(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    player_stop(&a->player);
    return 0;
}

static int do_next(struct tune_queue_app_state *a, char **s)
{
    (void)s;
    app_advance(a, "skipped");
    return 0;
}

static int do_seek(struct tune_queue_app_state *a, char **s)
{
    char *arg = strtok_r(NULL, " \t\r\n", s);
    if (arg) {
        player_seek(&a->player, atof(arg));
    }
    return 0;
}

static int do_vol(struct tune_queue_app_state *a, char **s)
{
    char *arg = strtok_r(NULL, " \t\r\n", s);
    if (arg) {
        player_set_volume(&a->player, atof(arg));
    }
    return 0;
}

static int do_like(struct tune_queue_app_state *a, char **s)
{
    char *arg = strtok_r(NULL, " \t\r\n", s);
    int i = arg ? atoi(arg) - 1 : -1;
    if (i >= 0 && i < a->queue.count) {
        struct queue_item *it = &a->queue.items[i];
        app_set_like(a, it->video.id, !it->liked);
        app_reload(a);
    } else {
        puts("like: index out of range");
    }
    return 0;
}

static int do_add(struct tune_queue_app_state *a, char **s)
{
    const char **urls = NULL;
    int n = 0, cap = 0;
    char *u;
    while ((u = strtok_r(NULL, " \t\r\n", s))) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            urls = realloc(urls, cap * sizeof(*urls));
        }
        urls[n++] = u;
    }
    if (n == 0) {
        free(urls);
        puts("add: need a url");
        return 0;
    }
    struct add_result r = app_add_urls(a, urls, n);
    free(urls);
    app_reload(a);
    printf("added %d, %d duplicate(s)%s\n", r.added, r.duplicates,
           r.error_count ? ", some errors" : "");
    return 0;
}

static int do_play(struct tune_queue_app_state *a, char **s)
{
    char *arg = strtok_r(NULL, " \t\r\n", s);
    if (arg) {
        int i = atoi(arg) - 1;
        if (i >= 0 && i < a->queue.count) {
            app_play_queue_item(a, &a->queue.items[i]);
        } else {
            puts("play: index out of range");
        }
    } else {
        app_press_play(a);
    }
    return 0;
}

static int do_quit(struct tune_queue_app_state *a, char **s)
{
    (void)a;
    (void)s;
    return 1;
}

static int do_help(struct tune_queue_app_state *a, char **s)
{
    (void)a;
    (void)s;
    puts("commands:");
    for (const struct command *c = CMDS; c->name; c++) {
        char usage[64];
        snprintf(usage, sizeof(usage), "%s %s", c->name, c->args);
        printf("  %-22s %s\n", usage, c->desc);
    }
    return 0;
}

static const struct command CMDS[] = {
    { "queue", "", "list the queue", do_queue },
    { "liked", "", "list liked tracks", do_liked },
    { "history", "", "list play history", do_history },
    { "add", "<url>...", "queue videos", do_add },
    { "play", "[n]", "play item n, or resume the current", do_play },
    { "pause", "", "toggle pause", do_pause },
    { "next", "", "skip to the next track", do_next },
    { "stop", "", "stop playback", do_stop },
    { "seek", "<sec>", "seek to position", do_seek },
    { "vol", "<0-100>", "set volume", do_vol },
    { "like", "<n>", "toggle like on queue item n", do_like },
    { "now", "", "show the current track", do_now },
    { "reload", "", "reload from the database", do_reload },
    { "help", "", "show this help", do_help },
    { "quit", "", "exit", do_quit },
    { 0 },
};

static int dispatch(struct tune_queue_app_state *a, char *line)
{
    char *save, *cmd = strtok_r(line, " \t\r\n", &save);
    if (!cmd) {
        return 0;
    }
    if (!strcmp(cmd, "exit")) {
        cmd = "quit";
    } else if (!strcmp(cmd, "?")) {
        cmd = "help";
    }
    for (const struct command *c = CMDS; c->name; c++) {
        if (!strcmp(cmd, c->name)) {
            return c->fn(a, &save);
        }
    }
    printf("unknown command: %s (try help)\n", cmd);
    return 0;
}

// seconds elapsed since the previous call, for the player's progress accounting
static double tick_dt(void)
{
    static struct timespec prev;
    static int have;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt =
        have ? (now.tv_sec - prev.tv_sec) + (now.tv_nsec - prev.tv_nsec) / 1e9
             : 0;
    prev = now;
    have = 1;
    return dt;
}

int main(void)
{
    struct tune_queue_app_state app;
    char db_file[1024];
    home_join(TUNE_QUEUE_DEFAULT_DATA_DIR "/data.db", db_file, sizeof(db_file));

    struct tune_queue_app_config cfg = {
        .core = {
            .db_path = db_file,
            .ytdl_path = "yt-dlp",
            .heartbeat_interval = TUNE_QUEUE_DEFAULT_HEARTBEAT_INTERVAL,
        },
    };
    app_init(&app, &cfg);
    puts("TuneQueue CLI. Type help for commands.");
    cmd_queue(&app);

    char shown[16] = "";
    char *line = NULL;
    size_t line_cap = 0;
    int quit = 0;
    while (!quit) {
        fputs("> ", stdout);
        fflush(stdout);

        for (;;) {  // poll the player until a line of input is ready
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv = { 0, 100 * 1000 };
            int r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

            if (app_poll(&app, tick_dt()) == TUNE_QUEUE_APP_TRACK_FAILED) {
                printf("\r! could not load \"%s\": %s\n> ",
                       app.now.title.len ? str_c(&app.now.title) : app.now.id,
                       app.player.fail_msg[0] ? app.player.fail_msg
                                              : "unresolved");
            }

            const char *id = app.has_now ? app.now.id : "";
            if (app.player.status == PS_PLAYING && strcmp(shown, id) != 0) {
                snprintf(shown, sizeof(shown), "%s", id);
                printf("\r♪ %s\n> ",
                       app.now.title.len ? str_c(&app.now.title) : app.now.id);
                fflush(stdout);
            }
            if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
                break;
            }
        }

        if (getline(&line, &line_cap, stdin) < 0) {
            break;  // EOF
        }
        quit = dispatch(&app, line);
    }

    free(line);
    app_shutdown(&app);
    return 0;
}
