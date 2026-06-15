#include "helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define DIR_MODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

void home_join(const char *sub, char *out, int n)
{
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/%s", home ? home : ".", sub);
}

void mkdir_p(const char *dir)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, DIR_MODE);
            *p = '/';
        }
    }
    mkdir(tmp, DIR_MODE);
}

unsigned char *read_file(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (sz > 0) ? malloc(sz) : NULL;
    if (buf && fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        *len = (int)sz;
    }
    return buf;
}
