#ifndef DB_H
#define DB_H

#include <sqlite3.h>
#include "model.h"

struct queue_list {
    struct queue_item *items;
    int count;
};
struct history_list {
    struct history_entry *items;
    int count;
};
struct liked_list {
    struct liked_entry *items;
    int count;
};

struct add_result {
    int added;
    int duplicates;
    char errors[1024];  // newline-joined "url — message"
    int error_count;
};

// open (creating if needed) the database at an explicit path; the caller owns
// path resolution and ensures the parent directory exists
sqlite3 *db_open(const char *path);

struct queue_list db_list_queue(sqlite3 *db);
void queue_list_free(struct queue_list *l);

struct add_result db_add_to_queue(sqlite3 *db, const char **urls, int n);
int db_remove_from_queue(sqlite3 *db,
                         const char *video_id);  // nonzero on failure

void db_abandon_open_sessions(sqlite3 *db, const char *video_id);
long long db_start_session(sqlite3 *db, const char *video_id);  // -1 if unknown
void db_heartbeat(sqlite3 *db, long long sid, double seconds, double position,
                  double duration);
// finish: returns 1 if a next item exists (filled into *next), else 0
int db_finish_session(sqlite3 *db, long long sid, const char *outcome,
                      double seconds, double position, struct queue_item *next);

struct history_list db_list_history(sqlite3 *db, int limit, int offset);
void history_list_free(struct history_list *l);

struct stats db_stats(sqlite3 *db);
void stats_free(struct stats *s);

struct liked_list db_list_likes(sqlite3 *db);
void liked_list_free(struct liked_list *l);
int db_add_like(sqlite3 *db, const char *video_id);     // nonzero on failure
int db_remove_like(sqlite3 *db, const char *video_id);  // nonzero on failure

int db_has_api_key(sqlite3 *db);
char *db_api_key(sqlite3 *db);  // malloc'd or NULL
int db_put_api_key(sqlite3 *db, const char *key, char *err, int errlen);

#endif
