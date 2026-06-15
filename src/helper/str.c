#include "str.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void str_reserve(struct str *s, int need)
{
    if (s->cap >= need) {
        return;
    }
    int cap = s->cap > 0 ? s->cap : 16;
    while (cap < need) {
        if (cap > INT_MAX / 2) {
            abort();
        }
        cap *= 2;
    }
    char *data = realloc(s->data, cap);
    if (!data) {
        abort();
    }
    if (!s->data) {
        data[0] = '\0';
    }
    s->data = data;
    s->cap = cap;
}

struct str str_from(const char *s)
{
    struct str out = { 0 };
    str_reserve(&out, 1);
    if (s) {
        str_append(&out, s);
    }
    return out;
}

void str_append(struct str *s, const char *add)
{
    if (!add) {
        return;
    }
    int n = (int)strlen(add);
    str_reserve(s, s->len + n + 1);
    memcpy(s->data + s->len, add, n + 1);
    s->len += n;
}

void str_appendf(struct str *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list probe;
    va_copy(probe, ap);
    int n = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (n < 0) {
        va_end(ap);
        return;
    }
    str_reserve(s, s->len + n + 1);
    vsnprintf(s->data + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    s->len += n;
}

void str_free(struct str *s)
{
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}
