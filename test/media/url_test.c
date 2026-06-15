// URL and duration parsing — pure, no network or disk
#include <stdio.h>
#include <string.h>

#include "youtube.h"

static int fails = 0;

static void check_id(const char *url, const char *want)
{
    char out[16] = { 0 };
    int ok = yt_extract_video_id(url, out);
    int pass = want ? (ok && strcmp(out, want) == 0) : (!ok);
    printf("  [%s] extract(%.50s) = %s\n", pass ? "ok" : "FAIL", url,
           ok ? out : "(none)");
    if (!pass) {
        fails++;
    }
}

static void check_dur(const char *s, long want)
{
    long g = yt_parse_iso8601_duration(s);
    int pass = g == want;
    printf("  [%s] dur(%s) = %ld (want %ld)\n", pass ? "ok" : "FAIL", s, g,
           want);
    if (!pass) {
        fails++;
    }
}

int main(void)
{
    printf("== url parsing ==\n");
    check_id("https://www.youtube.com/watch?v=qP0dSxLOTEc", "qP0dSxLOTEc");
    check_id(
        "https://www.youtube.com/"
        "watch?v=PQQqU4h8JBs&list=RDPQQqU4h8JBs&index=2",
        "PQQqU4h8JBs");
    check_id("https://youtu.be/qP0dSxLOTEc?si=abc", "qP0dSxLOTEc");
    check_id("https://www.youtube.com/shorts/qP0dSxLOTEc", "qP0dSxLOTEc");
    check_id("https://www.youtube.com/embed/qP0dSxLOTEc", "qP0dSxLOTEc");
    check_id("https://www.youtube.com/live/qP0dSxLOTEc", "qP0dSxLOTEc");
    check_id("https://music.youtube.com/watch?v=qP0dSxLOTEc", "qP0dSxLOTEc");
    check_id("qP0dSxLOTEc", "qP0dSxLOTEc");
    check_id("https://example.com/watch?v=qP0dSxLOTEc", NULL);
    check_id("https://www.youtube.com/playlist?list=PL123", NULL);
    check_id("not a url", NULL);

    printf("== duration parsing ==\n");
    check_dur("PT1H2M3S", 3723);
    check_dur("PT4M13S", 253);
    check_dur("PT45S", 45);
    check_dur("P1DT2H", 93600);
    check_dur("", -1);
    check_dur("1:02:03", -1);

    printf("%s (%d failures)\n", fails ? "FAILURES" : "all ok", fails);
    return fails ? 1 : 0;
}
