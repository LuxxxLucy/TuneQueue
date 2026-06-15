#ifndef TUNE_QUEUE_STR_H
#define TUNE_QUEUE_STR_H

struct str {
    char *data;
    int len;
    int cap;
};

struct str str_from(const char *s);
void str_append(struct str *s, const char *add);
void str_appendf(struct str *s, const char *fmt, ...);
void str_free(struct str *s);

static inline const char *str_c(const struct str *s)
{
    return (s && s->data) ? s->data : "";
}

#endif
