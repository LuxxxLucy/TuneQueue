// Data layer — opens a throwaway database, seeds a couple of queue rows, and
// reads them back through db_list_queue. Never touches the real database.
#include <stdio.h>
#include <unistd.h>

#include "db.h"
#include "tmpdb.h"

int main(void)
{
    printf("== db ==\n");
    char dbp[512];
    tmpdb_path(dbp, sizeof(dbp));
    unlink(dbp);

    sqlite3 *db = db_open(dbp);
    if (!db) {
        printf("  [FAIL] db_open returned NULL\n");
        return 1;
    }
    sqlite3_exec(
        db,
        "INSERT INTO videos(id,url,title,metadata_source) VALUES"
        "('tqDbTest001','https://youtu.be/tqDbTest001','First','none'),"
        "('tqDbTest002','https://youtu.be/tqDbTest002','Second','none');"
        "INSERT INTO queue_items(video_id,position) VALUES"
        "('tqDbTest001',1),('tqDbTest002',2);",
        0, 0, 0);

    struct queue_list q = db_list_queue(db);
    int ok = q.count == 2;
    printf("  [%s] db_list_queue returned the %d seeded rows\n",
           ok ? "ok" : "FAIL", q.count);
    for (int i = 0; i < q.count; i++) {
        printf("    %d. [%s] %s\n", i + 1, q.items[i].video.id,
               q.items[i].video.title.len ? str_c(&q.items[i].video.title)
                                           : "(no title)");
    }
    queue_list_free(&q);
    sqlite3_close(db);
    unlink(dbp);

    printf("%s\n", ok ? "all ok" : "FAILURES");
    return ok ? 0 : 1;
}
