#ifndef TUNE_QUEUE_TMPDB_H
#define TUNE_QUEUE_TMPDB_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// A throwaway database path, unique to this process. Tests open the database
// here instead of resolving the real application path, so a test can never read
// or modify the user's queue even when run directly.
static inline void tmpdb_path(char *out, int n)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) {
        tmp = "/tmp";
    }
    snprintf(out, n, "%s/tunequeue-test-%d.db", tmp, (int)getpid());
}

#endif
