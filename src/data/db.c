#include "db.h"
#include "youtube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS videos ("
    "  id TEXT PRIMARY KEY, url TEXT NOT NULL, title TEXT, channel_title TEXT,"
    "  description TEXT, thumbnail_url TEXT, duration_seconds INTEGER,"
    "  metadata_source TEXT NOT NULL DEFAULT 'none', fetched_at TEXT,"
    "  created_at TEXT NOT NULL DEFAULT (datetime('now')));"
    "CREATE TABLE IF NOT EXISTS queue_items ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  video_id TEXT NOT NULL UNIQUE REFERENCES videos(id),"
    "  position INTEGER NOT NULL, added_at TEXT NOT NULL DEFAULT "
    "(datetime('now')));"
    "CREATE TABLE IF NOT EXISTS listen_sessions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, video_id TEXT NOT NULL REFERENCES "
    "videos(id),"
    "  started_at TEXT NOT NULL DEFAULT (datetime('now')), ended_at TEXT,"
    "  seconds_listened REAL NOT NULL DEFAULT 0, max_position REAL NOT NULL "
    "DEFAULT 0, outcome TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_video ON "
    "listen_sessions(video_id);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_started ON "
    "listen_sessions(started_at);"
    "CREATE TABLE IF NOT EXISTS likes ("
    "  video_id TEXT PRIMARY KEY REFERENCES videos(id),"
    "  created_at TEXT NOT NULL DEFAULT (datetime('now')));"
    "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT "
    "NULL);";

#define QUEUE_SELECT                                                         \
    "SELECT q.id AS queue_id, q.added_at, v.*, "                             \
    "(b.video_id IS NOT NULL) AS liked, "                                    \
    "(SELECT s.max_position FROM listen_sessions s WHERE s.video_id = v.id " \
    "   AND s.outcome = 'abandoned' AND s.max_position > 1 "                 \
    "   ORDER BY s.started_at DESC, s.id DESC LIMIT 1) AS resume_position "  \
    "FROM queue_items q JOIN videos v ON v.id = q.video_id "                 \
    "LEFT JOIN likes b ON b.video_id = v.id"

static const char *UPSERT_VIDEO_SQL =
    "INSERT INTO videos (id, url, title, channel_title, description, "
    "thumbnail_url,"
    " duration_seconds, metadata_source, fetched_at) VALUES "
    "(?1,?2,?3,?4,?5,?6,?7,?8,datetime('now'))"
    " ON CONFLICT(id) DO UPDATE SET title=excluded.title, "
    "channel_title=excluded.channel_title,"
    " description=excluded.description, thumbnail_url=excluded.thumbnail_url,"
    " duration_seconds=excluded.duration_seconds, "
    "metadata_source=excluded.metadata_source,"
    " fetched_at=excluded.fetched_at";

sqlite3 *db_open(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "open database failed: %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", 0, 0,
                 0);
    sqlite3_exec(db, SCHEMA, 0, 0, 0);
    // close out sessions left open by an unclean exit so resume still works
    sqlite3_exec(db,
                 "UPDATE listen_sessions SET outcome='abandoned', "
                 "ended_at=datetime('now') WHERE outcome IS NULL",
                 0, 0, 0);
    return db;
}

// --- column helpers ---
static int col(sqlite3_stmt *st, const char *name)
{
    int n = sqlite3_column_count(st);
    for (int i = 0; i < n; i++) {
        const char *c = sqlite3_column_name(st, i);
        if (c && strcmp(c, name) == 0) {
            return i;
        }
    }
    return -1;
}

static char *dup_col(sqlite3_stmt *st, const char *name)
{
    int i = col(st, name);
    if (i < 0 || sqlite3_column_type(st, i) == SQLITE_NULL) {
        return NULL;
    }
    const unsigned char *t = sqlite3_column_text(st, i);
    return t ? strdup((const char *)t) : NULL;
}

static struct str str_col(sqlite3_stmt *st, const char *name)
{
    int i = col(st, name);
    if (i < 0 || sqlite3_column_type(st, i) == SQLITE_NULL) {
        return str_from(NULL);
    }
    const unsigned char *t = sqlite3_column_text(st, i);
    return str_from(t ? (const char *)t : NULL);
}

static void video_from_stmt(sqlite3_stmt *st, struct video *v)
{
    memset(v, 0, sizeof(*v));
    char *id = dup_col(st, "id");
    if (id) {
        strncpy(v->id, id, sizeof(v->id) - 1);
        free(id);
    }
    v->url = str_col(st, "url");
    v->title = str_col(st, "title");
    v->channel_title = str_col(st, "channel_title");
    v->description = str_col(st, "description");
    v->thumbnail_url = str_col(st, "thumbnail_url");
    int di = col(st, "duration_seconds");
    v->duration_seconds =
        (di >= 0 && sqlite3_column_type(st, di) != SQLITE_NULL)
            ? sqlite3_column_int64(st, di)
            : -1;
    char *src = dup_col(st, "metadata_source");
    if (src) {
        strncpy(v->metadata_source, src, sizeof(v->metadata_source) - 1);
        free(src);
    }
}

void video_free(struct video *v)
{
    str_free(&v->url);
    str_free(&v->title);
    str_free(&v->channel_title);
    str_free(&v->description);
    str_free(&v->thumbnail_url);
}

static void queue_item_from_stmt(sqlite3_stmt *st, struct queue_item *it)
{
    memset(it, 0, sizeof(*it));
    it->queue_id = sqlite3_column_int64(st, col(st, "queue_id"));
    it->added_at = dup_col(st, "added_at");
    it->liked = sqlite3_column_int(st, col(st, "liked")) != 0;
    int ri = col(st, "resume_position");
    if (ri >= 0 && sqlite3_column_type(st, ri) != SQLITE_NULL) {
        it->has_resume = 1;
        it->resume_position = sqlite3_column_double(st, ri);
    }
    video_from_stmt(st, &it->video);
}

// run a write statement binding a single text parameter
// 0 on success, else the sqlite result code
static int exec_text(sqlite3 *db, const char *sql, const char *arg)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, sql, -1, &st, 0);
    sqlite3_bind_text(st, 1, arg, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : rc;
}

static void bind_str_or_null(sqlite3_stmt *st, int idx, const struct str *s)
{
    if (s->len > 0) {
        sqlite3_bind_text(st, idx, str_c(s), -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, idx);
    }
}

// insert or refresh a video row from fetched metadata
static void upsert_video(sqlite3 *db, const char *id,
                         const struct video_meta *m)
{
    char wurl[YT_WATCH_URL_MAX];
    yt_watch_url(id, wurl);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, UPSERT_VIDEO_SQL, -1, &st, 0);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, wurl, -1, SQLITE_TRANSIENT);
    bind_str_or_null(st, 3, &m->title);
    bind_str_or_null(st, 4, &m->channel_title);
    bind_str_or_null(st, 5, &m->description);
    bind_str_or_null(st, 6, &m->thumbnail_url);
    if (m->duration_seconds >= 0) {
        sqlite3_bind_int64(st, 7, m->duration_seconds);
    } else {
        sqlite3_bind_null(st, 7);
    }
    sqlite3_bind_text(st, 8, m->source, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// --- queue ---
struct queue_list db_list_queue(sqlite3 *db)
{
    struct queue_list l = { NULL, 0 };
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, QUEUE_SELECT " ORDER BY q.position", -1, &st,
                           0) != SQLITE_OK) {
        return l;
    }
    int cap = 16;
    l.items = malloc(cap * sizeof(struct queue_item));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (l.count == cap) {
            cap *= 2;
            l.items = realloc(l.items, cap * sizeof(struct queue_item));
        }
        queue_item_from_stmt(st, &l.items[l.count++]);
    }
    sqlite3_finalize(st);
    return l;
}

void queue_list_free(struct queue_list *l)
{
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].added_at);
        video_free(&l->items[i].video);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static int video_exists(sqlite3 *db, const char *id)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT 1 FROM videos WHERE id=?1", -1, &st, 0);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    int found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

struct add_result db_add_to_queue(sqlite3 *db, const char **urls, int n)
{
    struct add_result res = { 0, 0, "", 0 };
    char ids[256][16];
    int nids = 0;
    for (int i = 0; i < n; i++) {
        const char *u = urls[i];
        if (!u) {
            continue;
        }
        while (*u == ' ' || *u == '\t' || *u == '\n' || *u == '\r') {
            u++;
        }
        if (!*u) {
            continue;
        }
        char vid[16];
        if (!yt_extract_video_id(u, vid)) {
            if (res.error_count < 8) {
                size_t len = strlen(res.errors);
                snprintf(res.errors + len, sizeof(res.errors) - len,
                         "%s%.60s — not a recognizable YouTube video URL",
                         res.error_count ? "\n" : "", u);
            }
            res.error_count++;
            continue;
        }
        int dup = 0;
        for (int k = 0; k < nids; k++) {
            if (strcmp(ids[k], vid) == 0) {
                dup = 1;
            }
        }
        if (!dup && nids < 256) {
            strcpy(ids[nids++], vid);
        }
    }

    // which need a metadata fetch
    const char *needs[256];
    int nneeds = 0;
    for (int i = 0; i < nids; i++) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db, "SELECT metadata_source FROM videos WHERE id=?1",
                           -1, &st, 0);
        sqlite3_bind_text(st, 1, ids[i], -1, SQLITE_STATIC);
        const char *src = NULL;
        if (sqlite3_step(st) == SQLITE_ROW) {
            src = (const char *)sqlite3_column_text(st, 0);
        }
        int fetch = (src == NULL) || strcmp(src, "none") == 0;
        sqlite3_finalize(st);
        if (fetch) {
            needs[nneeds++] = ids[i];
        }
    }

    struct video_meta metas[256];
    char *key = db_api_key(db);
    if (nneeds > 0) {
        yt_fetch_metadata(needs, nneeds, key, metas);
    }
    free(key);

    long long next_pos = 1;
    sqlite3_stmt *mp;
    sqlite3_prepare_v2(
        db, "SELECT COALESCE(MAX(position),0)+1 FROM queue_items", -1, &mp, 0);
    if (sqlite3_step(mp) == SQLITE_ROW) {
        next_pos = sqlite3_column_int64(mp, 0);
    }
    sqlite3_finalize(mp);

    for (int i = 0; i < nids; i++) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db, "SELECT 1 FROM queue_items WHERE video_id=?1",
                           -1, &st, 0);
        sqlite3_bind_text(st, 1, ids[i], -1, SQLITE_STATIC);
        int already = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (already) {
            res.duplicates++;
            continue;
        }

        int is_need = 0, need_idx = -1;
        for (int k = 0; k < nneeds; k++) {
            if (needs[k] == ids[i] || strcmp(needs[k], ids[i]) == 0) {
                is_need = 1;
                need_idx = k;
                break;
            }
        }
        struct video_meta *m = (is_need && need_idx >= 0 && metas[need_idx].ok)
                                   ? &metas[need_idx]
                                   : NULL;
        char wurl[YT_WATCH_URL_MAX];
        yt_watch_url(ids[i], wurl);
        if (is_need && !m) {
            sqlite3_prepare_v2(
                db, "INSERT OR IGNORE INTO videos (id,url) VALUES (?1,?2)", -1,
                &st, 0);
            sqlite3_bind_text(st, 1, ids[i], -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, wurl, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        } else if (m) {
            upsert_video(db, ids[i], m);
        }
        sqlite3_prepare_v2(
            db, "INSERT INTO queue_items (video_id,position) VALUES (?1,?2)",
            -1, &st, 0);
        sqlite3_bind_text(st, 1, ids[i], -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, next_pos++);
        sqlite3_step(st);
        sqlite3_finalize(st);
        res.added++;
    }
    for (int k = 0; k < nneeds; k++) {
        yt_meta_free(&metas[k]);
    }
    return res;
}

// --- sessions ---
void db_abandon_open_sessions(sqlite3 *db, const char *video_id)
{
    const char *base =
        "UPDATE listen_sessions SET outcome='abandoned', "
        "ended_at=datetime('now') WHERE outcome IS NULL";
    if (video_id) {
        char sql[256];
        snprintf(sql, sizeof(sql), "%s AND video_id=?1", base);
        exec_text(db, sql, video_id);
    } else {
        sqlite3_exec(db, base, 0, 0, 0);
    }
}

long long db_start_session(sqlite3 *db, const char *video_id)
{
    if (!video_exists(db, video_id)) {
        return -1;
    }
    db_abandon_open_sessions(db, video_id);
    exec_text(db, "INSERT INTO listen_sessions (video_id) VALUES (?1)",
              video_id);
    return sqlite3_last_insert_rowid(db);
}

// return video_id (malloc) and outcome-present flag for a session; NULL if
// unknown
static char *session_lookup(sqlite3 *db, long long sid, int *has_outcome)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(
        db, "SELECT video_id, outcome FROM listen_sessions WHERE id=?1", -1,
        &st, 0);
    sqlite3_bind_int64(st, 1, sid);
    char *vid = NULL;
    *has_outcome = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        vid = strdup((const char *)sqlite3_column_text(st, 0));
        *has_outcome = sqlite3_column_type(st, 1) != SQLITE_NULL;
    }
    sqlite3_finalize(st);
    return vid;
}

void db_heartbeat(sqlite3 *db, long long sid, double seconds, double position,
                  double duration)
{
    int has_outcome;
    char *vid = session_lookup(db, sid, &has_outcome);
    if (!vid) {
        return;
    }
    if (has_outcome) {
        free(vid);
        return;
    }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(
        db,
        "UPDATE listen_sessions SET seconds_listened=MAX(seconds_listened,?1), "
        "max_position=MAX(max_position,?2) WHERE id=?3",
        -1, &st, 0);
    sqlite3_bind_double(st, 1, seconds);
    sqlite3_bind_double(st, 2, position);
    sqlite3_bind_int64(st, 3, sid);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (duration > 0) {
        sqlite3_prepare_v2(db,
                           "UPDATE videos SET duration_seconds=?1 WHERE id=?2 "
                           "AND duration_seconds IS NULL",
                           -1, &st, 0);
        sqlite3_bind_int64(st, 1, (long long)(duration + 0.5));
        sqlite3_bind_text(st, 2, vid, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    free(vid);
}

static int next_queue_item(sqlite3 *db, int has_after, long long after,
                           struct queue_item *out);

int db_finish_session(sqlite3 *db, long long sid, const char *outcome,
                      double seconds, double position, struct queue_item *next)
{
    if (strcmp(outcome, "completed") != 0 && strcmp(outcome, "skipped") != 0) {
        return 0;
    }
    int has_outcome;
    char *vid = session_lookup(db, sid, &has_outcome);
    if (!vid) {
        return 0;
    }
    int has_old_pos = 0;
    long long old_pos = 0;
    if (!has_outcome) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db,
                           "SELECT position FROM queue_items WHERE video_id=?1",
                           -1, &st, 0);
        sqlite3_bind_text(st, 1, vid, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            has_old_pos = 1;
            old_pos = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        sqlite3_prepare_v2(
            db,
            "UPDATE listen_sessions SET outcome=?1, ended_at=datetime('now'), "
            "seconds_listened=MAX(seconds_listened,?2), "
            "max_position=MAX(max_position,?3) "
            "WHERE id=?4",
            -1, &st, 0);
        sqlite3_bind_text(st, 1, outcome, -1, SQLITE_STATIC);
        sqlite3_bind_double(st, 2, seconds);
        sqlite3_bind_double(st, 3, position);
        sqlite3_bind_int64(st, 4, sid);
        sqlite3_step(st);
        sqlite3_finalize(st);
        exec_text(db, "DELETE FROM queue_items WHERE video_id=?1", vid);
    }
    free(vid);
    return next_queue_item(db, has_old_pos, old_pos, next);
}

static int next_queue_item(sqlite3 *db, int has_after, long long after,
                           struct queue_item *out)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(
        db,
        QUEUE_SELECT
        " WHERE q.position > COALESCE(?1,0) ORDER BY q.position LIMIT 1",
        -1, &st, 0);
    if (has_after) {
        sqlite3_bind_int64(st, 1, after);
    } else {
        sqlite3_bind_null(st, 1);
    }
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        queue_item_from_stmt(st, out);
        found = 1;
    }
    sqlite3_finalize(st);
    if (!found && has_after) {
        return next_queue_item(db, 0, 0, out);
    }
    return found;
}

// --- history + stats ---
struct history_list db_list_history(sqlite3 *db, int limit, int offset)
{
    struct history_list l = { NULL, 0 };
    sqlite3_stmt *st;
    const char *sql =
        "SELECT s.id AS session_id, s.started_at, s.ended_at, "
        "s.seconds_listened, s.max_position, s.outcome,"
        " v.*, (b.video_id IS NOT NULL) AS liked, (q.video_id IS NOT NULL) AS "
        "queued "
        "FROM listen_sessions s JOIN videos v ON v.id=s.video_id "
        "LEFT JOIN likes b ON b.video_id=v.id LEFT JOIN queue_items q ON "
        "q.video_id=v.id "
        "WHERE s.outcome IS NOT NULL ORDER BY s.started_at DESC, s.id DESC "
        "LIMIT ?1 OFFSET ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) {
        return l;
    }
    sqlite3_bind_int(st, 1, limit);
    sqlite3_bind_int(st, 2, offset);
    int cap = 16;
    l.items = malloc(cap * sizeof(struct history_entry));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (l.count == cap) {
            cap *= 2;
            l.items = realloc(l.items, cap * sizeof(struct history_entry));
        }
        struct history_entry *e = &l.items[l.count++];
        memset(e, 0, sizeof(*e));
        e->session_id = sqlite3_column_int64(st, col(st, "session_id"));
        e->started_at = dup_col(st, "started_at");
        e->ended_at = dup_col(st, "ended_at");
        e->seconds_listened =
            sqlite3_column_double(st, col(st, "seconds_listened"));
        e->max_position = sqlite3_column_double(st, col(st, "max_position"));
        e->outcome = dup_col(st, "outcome");
        e->liked = sqlite3_column_int(st, col(st, "liked")) != 0;
        e->queued = sqlite3_column_int(st, col(st, "queued")) != 0;
        video_from_stmt(st, &e->video);
    }
    sqlite3_finalize(st);
    return l;
}

void history_list_free(struct history_list *l)
{
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].started_at);
        free(l->items[i].ended_at);
        free(l->items[i].outcome);
        video_free(&l->items[i].video);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

struct stats db_stats(sqlite3 *db)
{
    struct stats s = { 0 };
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
                       "SELECT CAST(COALESCE(SUM(seconds_listened),0) AS "
                       "REAL), COUNT(DISTINCT video_id),"
                       " SUM(outcome='completed'), SUM(outcome='skipped') FROM "
                       "listen_sessions",
                       -1, &st, 0);
    if (sqlite3_step(st) == SQLITE_ROW) {
        s.total_seconds = sqlite3_column_double(st, 0);
        s.total_videos = sqlite3_column_int64(st, 1);
        s.completed_count = sqlite3_column_int64(st, 2);
        s.skipped_count = sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    sqlite3_prepare_v2(
        db,
        "SELECT date(started_at) AS date, CAST(SUM(seconds_listened) AS REAL) "
        "AS seconds "
        "FROM listen_sessions WHERE started_at >= date('now','-29 days') "
        "GROUP BY date(started_at) ORDER BY date",
        -1, &st, 0);
    int cap = 32;
    s.per_day = malloc(cap * sizeof(struct day_stat));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (s.per_day_count == cap) {
            cap *= 2;
            s.per_day = realloc(s.per_day, cap * sizeof(struct day_stat));
        }
        struct day_stat *d = &s.per_day[s.per_day_count++];
        const char *date = (const char *)sqlite3_column_text(st, 0);
        snprintf(d->date, sizeof(d->date), "%s", date ? date : "");
        d->seconds = sqlite3_column_double(st, 1);
    }
    sqlite3_finalize(st);
    return s;
}

void stats_free(struct stats *s)
{
    free(s->per_day);
    s->per_day = NULL;
    s->per_day_count = 0;
}

// --- likes ---
struct liked_list db_list_likes(sqlite3 *db)
{
    struct liked_list l = { NULL, 0 };
    sqlite3_stmt *st;
    sqlite3_prepare_v2(
        db,
        "SELECT b.created_at AS liked_at, v.* FROM likes b "
        "JOIN videos v ON v.id=b.video_id ORDER BY b.created_at DESC",
        -1, &st, 0);
    int cap = 16;
    l.items = malloc(cap * sizeof(struct liked_entry));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (l.count == cap) {
            cap *= 2;
            l.items = realloc(l.items, cap * sizeof(struct liked_entry));
        }
        struct liked_entry *e = &l.items[l.count++];
        memset(e, 0, sizeof(*e));
        e->liked_at = dup_col(st, "liked_at");
        video_from_stmt(st, &e->video);
    }
    sqlite3_finalize(st);
    return l;
}

void liked_list_free(struct liked_list *l)
{
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].liked_at);
        video_free(&l->items[i].video);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

int db_add_like(sqlite3 *db, const char *video_id)
{
    if (!video_exists(db, video_id)) {
        return -1;
    }
    return exec_text(db, "INSERT OR IGNORE INTO likes (video_id) VALUES (?1)",
                     video_id);
}

int db_remove_like(sqlite3 *db, const char *video_id)
{
    return exec_text(db, "DELETE FROM likes WHERE video_id=?1", video_id);
}

// --- settings ---
static char *get_setting(sqlite3 *db, const char *key)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key=?1", -1, &st,
                       0);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    char *v = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) {
            v = strdup((const char *)t);
        }
    }
    sqlite3_finalize(st);
    return v;
}

char *db_api_key(sqlite3 *db)
{
    char *stored = get_setting(db, "api_key");
    if (stored && *stored) {
        return stored;
    }
    free(stored);
    return yt_get_api_key_env();
}

int db_has_api_key(sqlite3 *db)
{
    char *k = db_api_key(db);
    int has = k != NULL;
    free(k);
    return has;
}

static void backfill_metadata(sqlite3 *db, const char *key)
{
    char ids[256][16];
    int n = 0;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(
        db, "SELECT id FROM videos WHERE metadata_source != 'api'", -1, &st, 0);
    while (sqlite3_step(st) == SQLITE_ROW && n < 256) {
        strncpy(ids[n++], (const char *)sqlite3_column_text(st, 0), 15);
    }
    sqlite3_finalize(st);
    if (n == 0) {
        return;
    }
    const char *idp[256];
    for (int i = 0; i < n; i++) {
        idp[i] = ids[i];
    }
    struct video_meta metas[256];
    yt_fetch_metadata(idp, n, key, metas);
    for (int i = 0; i < n; i++) {
        if (!metas[i].ok) {
            continue;
        }
        upsert_video(db, ids[i], &metas[i]);
        yt_meta_free(&metas[i]);
    }
}

int db_put_api_key(sqlite3 *db, const char *key, char *err, int errlen)
{
    if (!key) {
        return 0;
    }
    while (*key == ' ' || *key == '\t') {
        key++;
    }
    if (!*key) {
        return 0;
    }
    if (!yt_validate_key(key, err, errlen)) {
        return 1;
    }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
                       "INSERT INTO settings (key,value) VALUES ('api_key',?1) "
                       "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                       -1, &st, 0);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
    backfill_metadata(db, key);
    return 0;
}
