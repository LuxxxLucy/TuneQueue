#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_renderer_raylib.c"
#pragma clang diagnostic pop

#include "app.h"
#include "helper.h"
#include "db.h"
#include "frames.h"
#include "player.h"
#include "thumbs.h"

#define FRAME_INTERVAL \
    300.0                // seconds between stage stills grabbed from the video
#define FRAME_FIRST 5.0  // first still, once playback is underway

static const Clay_Color BG = { 18, 18, 17, 255 };
static const Clay_Color SURFACE = { 26, 26, 24, 255 };
static const Clay_Color SURFACE2 = { 32, 31, 29, 255 };
static const Clay_Color SURFACE3 = { 42, 41, 37, 255 };
static const Clay_Color BORDER = { 47, 46, 42, 255 };
static const Clay_Color BORDER_STR = { 64, 62, 56, 255 };
static const Clay_Color TEXT = { 233, 230, 221, 255 };
static const Clay_Color DIM = { 168, 164, 150, 255 };
static const Clay_Color FAINT = { 111, 107, 95, 255 };
static const Clay_Color ACCENT = { 224, 139, 79, 255 };
static const Clay_Color ACCENT_SOFT = { 224, 139, 79, 38 };
static const Clay_Color ACCENT_TEXT = { 232, 168, 106, 255 };
static const Clay_Color INK = { 0, 0, 0, 255 };
static const Clay_Color WHITEC = { 255, 255, 255, 255 };

#define FONT_PATH "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
#define FONT_BASE 40

enum { VIEW_QUEUE, VIEW_LIKED, VIEW_HISTORY };

// the application core; the GUI reads its fields and mutates them only through
// the app_* interface
static struct tune_queue_app_state app;

// presentation-only state, owned entirely by the GUI
static int view = VIEW_QUEUE;
static int panel_open = 1;
static int add_open, settings_open;
static int focus;  // 0 none, 1 add, 2 key
static char add_text[4096];
static int add_len;
static char key_text[256];
static int key_len;
static char msg[256];
static int error_open;
static char error_msg[256];
static double frames_due;  // playing seconds until the next still
static Font fonts[1];
static const char *font_path;  // the UI font file, from the app config

// per-frame string pool; Clay stores slices, not copies
static char fpool[1 << 16];
static int fpos;
static void fpool_reset(void) { fpos = 0; }
static Clay_String fstr(const char *fmt, ...)
{
    char *dst = fpool + fpos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, sizeof(fpool) - fpos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    fpos += n + 1;
    return (Clay_String){ .isStaticallyAllocated = false,
                          .length = n,
                          .chars = dst };
}
static Clay_String cs(const char *s)
{
    return (Clay_String){ .isStaticallyAllocated = false,
                          .length = s ? (int)strlen(s) : 0,
                          .chars = s ? s : "" };
}

static void fmt_time(double sec, char *out, int n)
{
    if (sec < 0 || isnan(sec)) {
        sec = 0;
    }
    long t = (long)(sec + 0.5);
    long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0) {
        snprintf(out, n, "%ld:%02ld:%02ld", h, m, s);
    } else {
        snprintf(out, n, "%ld:%02ld", m, s);
    }
}

/*
 * The codepoint set is cumulative; the atlas is only re-rasterized when a new
 * glyph appears, so routine data reloads (a like toggle, a heartbeat) are free.
 */
static int *cps;
static int cps_cap, cps_n;
static int fonts_dirty;
static void cp_add(int c)
{
    for (int i = 0; i < cps_n; i++) {
        if (cps[i] == c) {
            return;
        }
    }
    if (cps_n == cps_cap) {
        cps_cap = cps_cap ? cps_cap * 2 : 1024;
        cps = realloc(cps, cps_cap * sizeof(int));
    }
    cps[cps_n++] = c;
    fonts_dirty = 1;
}
static void cp_add_text(const char *s)
{
    if (!s) {
        return;
    }
    int n = 0;
    int *list = LoadCodepoints(s, &n);
    for (int i = 0; i < n; i++) {
        cp_add(list[i]);
    }
    UnloadCodepoints(list);
}
static void cp_add_video(const struct video *v)
{
    cp_add_text(str_c(&v->title));
    cp_add_text(str_c(&v->channel_title));
    cp_add_text(str_c(&v->description));
}
static void ensure_font(void)
{
    static int base = 0;
    if (!base) {
        for (int c = 32; c < 127; c++) {
            cp_add(c);
        }
        cp_add_text("…—•·×♥▶■◀↺⚙⏭∞’“”‹›");  // ui glyphs we draw as text
        base = 1;
    }
    for (int i = 0; i < app.queue.count; i++) {
        cp_add_video(&app.queue.items[i].video);
    }
    for (int i = 0; i < app.history.count; i++) {
        cp_add_video(&app.history.items[i].video);
    }
    for (int i = 0; i < app.liked.count; i++) {
        cp_add_video(&app.liked.items[i].video);
    }
    if (app.has_now) {
        cp_add_video(&app.now);
    }
    if (!fonts_dirty) {
        return;
    }
    if (fonts[0].texture.id) {
        UnloadFont(fonts[0]);
    }
    fonts[0] = LoadFontEx(font_path, FONT_BASE, cps, cps_n);
    SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);
    Clay_ResetMeasureTextCache();
    fonts_dirty = 0;
}

// reload the lists, then fold any new glyphs into the font atlas
static void gui_reload(void)
{
    app_reload(&app);
    ensure_font();
}

static Clay_Dimensions measure(Clay_StringSlice text,
                               Clay_TextElementConfig *cfg, void *ud)
{
    Font *f = (Font *)ud;
    static char buf[2048];
    int n =
        text.length < (int)sizeof(buf) - 1 ? text.length : (int)sizeof(buf) - 1;
    memcpy(buf, text.chars, n);
    buf[n] = '\0';
    Vector2 sz = MeasureTextEx(f[cfg->fontId], buf, (float)cfg->fontSize,
                               (float)cfg->letterSpacing);
    return (Clay_Dimensions){ sz.x, sz.y };
}

static int g_clicked;
static int hit(Clay_ElementId id) { return g_clicked && Clay_PointerOver(id); }
static Clay_ElementId idi(const char *s, int i)
{
    return Clay_GetElementIdWithIndex(
        (Clay_String){ .length = (int)strlen(s), .chars = s }, (uint32_t)i);
}
static Clay_ElementId id_of(const char *s)
{
    return Clay_GetElementId(
        (Clay_String){ .length = (int)strlen(s), .chars = s });
}
static int hit_s(const char *s) { return hit(id_of(s)); }

static void text(Clay_String s, int size, Clay_Color color)
{
    CLAY_TEXT(s, CLAY_TEXT_CONFIG({ .fontSize = size,
                                    .fontId = 0,
                                    .textColor = color,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE }));
}

static void thumb_box(Clay_ElementId id, const char *vid, const char *url,
                      int w, int h)
{
    Texture2D *tx = thumbs_get(vid, url);
    CLAY(id, { .layout = { .sizing = { CLAY_SIZING_FIXED(w),
                                       CLAY_SIZING_FIXED(h) } },
               .backgroundColor = tx ? (Clay_Color){ 0, 0, 0, 0 } : SURFACE3,
               .cornerRadius = CLAY_CORNER_RADIUS(2),
               .image = tx ? (Clay_ImageElementConfig){ .imageData = tx }
                           : (Clay_ImageElementConfig){ 0 } })
    {
    }
}

static void track_row(const struct video *v, int liked, int playing, int index,
                      int show_dur, int has_resume)
{
    Clay_ElementId rid = idi("row", index);
    Clay_ElementId likeId = idi("like", index);
    Clay_ElementId playId = idi("play", index);
    Clay_Color bg =
        playing
            ? ACCENT_SOFT
            : (Clay_PointerOver(rid) ? SURFACE2 : (Clay_Color){ 0, 0, 0, 0 });
    CLAY(rid,
         { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                       .padding = { 8, 8, 7, 7 },
                       .childGap = 10,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = bg,
           .cornerRadius = CLAY_CORNER_RADIUS(3) })
    {
        thumb_box(idi("rowthumb", index), v->id, str_c(&v->thumbnail_url), 52,
                  32);
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                   .childGap = 2 } })
        {
            CLAY_TEXT(cs(v->title.len ? str_c(&v->title) : v->id),
                      CLAY_TEXT_CONFIG({ .fontSize = 14,
                                         .fontId = 0,
                                         .textColor = TEXT,
                                         .wrapMode = CLAY_TEXT_WRAP_WORDS }));
            if (v->channel_title.len || has_resume) {
                CLAY_AUTO_ID({ .layout = { .childGap = 6,
                                           .childAlignment = {
                                               .y = CLAY_ALIGN_Y_CENTER } } })
                {
                    if (v->channel_title.len) {
                        text(cs(str_c(&v->channel_title)), 12, DIM);
                    }
                    if (has_resume) {
                        text(CLAY_STRING("• resume"), 11, ACCENT_TEXT);
                    }
                }
            }
        }
        if (show_dur && v->duration_seconds >= 0) {
            char d[16];
            fmt_time(v->duration_seconds, d, sizeof(d));
            text(fstr("%s", d), 12, FAINT);
        }
        CLAY_AUTO_ID(
            { .layout = { .childGap = 2,
                          .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } })
        {
            CLAY(likeId,
                 { .layout = { .sizing = { CLAY_SIZING_FIXED(26),
                                           CLAY_SIZING_FIXED(26) },
                               .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                   CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = Clay_PointerOver(likeId)
                                          ? SURFACE3
                                          : (Clay_Color){ 0, 0, 0, 0 },
                   .cornerRadius = CLAY_CORNER_RADIUS(13) })
            {
                text(CLAY_STRING("♥"), 13, liked ? ACCENT_TEXT : FAINT);
            }
            CLAY(playId,
                 { .layout = { .sizing = { CLAY_SIZING_FIXED(26),
                                           CLAY_SIZING_FIXED(26) },
                               .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                   CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = Clay_PointerOver(playId)
                                          ? SURFACE3
                                          : (Clay_Color){ 0, 0, 0, 0 },
                   .cornerRadius = CLAY_CORNER_RADIUS(13) })
            {
                text(CLAY_STRING("▶"), 11, DIM);
            }
        }
    }
}

static void icon_button(const char *id_str, Clay_String glyph, int size,
                        int active, int big)
{
    Clay_ElementId id = (Clay_ElementId)Clay_GetElementId(
        (Clay_String){ .length = (int)strlen(id_str), .chars = id_str });
    int hover = Clay_PointerOver(id);
    int dim = big ? 44 : 30;
    Clay_Color bgc =
        big ? ACCENT : (hover ? SURFACE3 : (Clay_Color){ 0, 0, 0, 0 });
    Clay_Color fg = big ? WHITEC : (active ? ACCENT_TEXT : DIM);
    CLAY(id, { .layout = { .sizing = { CLAY_SIZING_FIXED(dim),
                                       CLAY_SIZING_FIXED(dim) },
                           .childAlignment = { CLAY_ALIGN_X_CENTER,
                                               CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = bgc,
               .cornerRadius = CLAY_CORNER_RADIUS(dim / 2) })
    {
        text(glyph, size, fg);
    }
}

static void tab(const char *id_str, Clay_String label, int active,
                int has_badge, int badge)
{
    Clay_ElementId id = Clay_GetElementId(
        (Clay_String){ .length = (int)strlen(id_str), .chars = id_str });
    CLAY(id,
         { .layout = { .padding = { 10, 10, 7, 7 },
                       .childGap = 7,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .color = active ? ACCENT : (Clay_Color){ 0, 0, 0, 0 },
                       .width = { .bottom = 2 } } })
    {
        text(label, 14,
             active ? ACCENT_TEXT : (Clay_PointerOver(id) ? TEXT : DIM));
        if (has_badge) {
            CLAY_AUTO_ID(
                { .layout = { .padding = { 6, 6, 1, 1 },
                              .childAlignment = { CLAY_ALIGN_X_CENTER } },
                  .backgroundColor = active ? ACCENT_SOFT : SURFACE3,
                  .cornerRadius = CLAY_CORNER_RADIUS(3) })
            {
                text(fstr("%d", badge), 11, active ? ACCENT_TEXT : DIM);
            }
        }
    }
}

static void build_list(void)
{
    CLAY(
        CLAY_ID("ListBody"),
        { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                      .layoutDirection = CLAY_TOP_TO_BOTTOM,
                      .padding = CLAY_PADDING_ALL(6),
                      .childGap = 2 },
          .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } })
    {
        if (view == VIEW_QUEUE) {
            if (app.queue.count == 0) {
                text(CLAY_STRING("Queue is empty. Press + to add videos."), 13,
                     FAINT);
            }
            for (int i = 0; i < app.queue.count; i++) {
                struct queue_item *it = &app.queue.items[i];
                int playing =
                    app.has_now && strcmp(app.now.id, it->video.id) == 0;
                track_row(&it->video, it->liked, playing, i, 1, it->has_resume);
            }
        } else if (view == VIEW_LIKED) {
            if (app.liked.count == 0) {
                text(CLAY_STRING("No liked videos yet."), 13, FAINT);
            }
            for (int i = 0; i < app.liked.count; i++) {
                track_row(&app.liked.items[i].video, 1, 0, i, 1, 0);
            }
        } else {
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                                       .padding = CLAY_PADDING_ALL(6),
                                       .childGap = 8 } })
            {
                CLAY_AUTO_ID(
                    { .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                                  .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                  .padding = CLAY_PADDING_ALL(10),
                                  .childGap = 3 },
                      .backgroundColor = SURFACE2,
                      .cornerRadius = CLAY_CORNER_RADIUS(3) })
                {
                    char hh[16];
                    fmt_time(app.stats.total_seconds, hh, sizeof(hh));
                    text(fstr("%s", hh), 19, TEXT);
                    text(CLAY_STRING("listening time"), 11, FAINT);
                }
                CLAY_AUTO_ID(
                    { .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                                  .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                  .padding = CLAY_PADDING_ALL(10),
                                  .childGap = 3 },
                      .backgroundColor = SURFACE2,
                      .cornerRadius = CLAY_CORNER_RADIUS(3) })
                {
                    text(fstr("%ld", app.stats.completed_count), 19, TEXT);
                    text(CLAY_STRING("completed"), 11, FAINT);
                }
            }
            if (app.history.count == 0) {
                text(CLAY_STRING("Nothing played yet."), 13, FAINT);
            }
            for (int i = 0; i < app.history.count; i++) {
                track_row(&app.history.items[i].video,
                          app.history.items[i].liked, 0, i, 1, 0);
            }
        }
    }
}

static void build_stage(void)
{
    CLAY(CLAY_ID("StageCol"),
         { .layout = {
               .sizing = { CLAY_SIZING_PERCENT(0.62f), CLAY_SIZING_GROW(0) },
               .layoutDirection = CLAY_TOP_TO_BOTTOM,
               .childGap = 12 } })
    {
        CLAY(CLAY_ID("Stage"),
             { .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                       CLAY_SIZING_GROW(0) },
                           .childAlignment = { CLAY_ALIGN_X_CENTER,
                                               CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = INK,
               .cornerRadius = CLAY_CORNER_RADIUS(4),
               .border = { .color = BORDER, .width = { 1, 1, 1, 1 } } })
        {
            if (!app.has_now) {
                text(CLAY_STRING("Nothing playing"), 14, FAINT);
            }
            // the frame or album art is drawn over the stage after Clay render
        }
        if (app.has_now) {
            CLAY(CLAY_ID("StageMeta"),
                 { .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                           CLAY_SIZING_FIT(.max = 220) },
                               .layoutDirection = CLAY_TOP_TO_BOTTOM,
                               .childGap = 4 },
                   .clip = { .vertical = true,
                             .childOffset = Clay_GetScrollOffset() } })
            {
                CLAY_TEXT(
                    cs(app.now.title.len ? str_c(&app.now.title) : app.now.id),
                    CLAY_TEXT_CONFIG({ .fontSize = 19,
                                       .fontId = 0,
                                       .textColor = TEXT,
                                       .wrapMode = CLAY_TEXT_WRAP_WORDS }));
                if (app.now.channel_title.len) {
                    text(cs(str_c(&app.now.channel_title)), 14, DIM);
                }
                if (app.now.description.len) {
                    CLAY_TEXT(
                        cs(str_c(&app.now.description)),
                        CLAY_TEXT_CONFIG({ .fontSize = 13,
                                           .fontId = 0,
                                           .textColor = DIM,
                                           .wrapMode = CLAY_TEXT_WRAP_WORDS,
                                           .lineHeight = 20 }));
                }
            }
        }
    }
}

static void build_panel(void)
{
    CLAY(CLAY_ID("Panel"),
         { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = SURFACE,
           .cornerRadius = CLAY_CORNER_RADIUS(4),
           .border = { .color = BORDER, .width = { 1, 1, 1, 1 } } })
    {
        CLAY(CLAY_ID("PanelHead"),
             { .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                       CLAY_SIZING_FIT(.min = 44) },
                           .padding = { 8, 8, 0, 0 },
                           .childGap = 4,
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .border = { .color = BORDER, .width = { .bottom = 1 } } })
        {
            tab("tabQueue", CLAY_STRING("Queue"), view == VIEW_QUEUE, 1,
                app.queue.count);
            tab("tabLiked", CLAY_STRING("Liked"), view == VIEW_LIKED, 0, 0);
            tab("tabHistory", CLAY_STRING("History"), view == VIEW_HISTORY, 0,
                0);
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}
            if (view == VIEW_QUEUE) {
                icon_button("addBtn", CLAY_STRING("+"), 18, 0, 0);
            }
        }
        build_list();
    }
}

// a small spinning glyph for the play button while a track loads
static Clay_String spinner(void)
{
    static const char *f[] = { "|", "/", "-", "\\" };
    return cs(f[(int)(GetTime() * 6) % 4]);
}

// where the cued track will resume from, before playback has started
static double now_resume(void)
{
    for (int i = 0; i < app.queue.count; i++) {
        if (strcmp(app.queue.items[i].video.id, app.now.id) == 0) {
            return app.queue.items[i].has_resume
                       ? app.queue.items[i].resume_position
                       : 0;
        }
    }
    return 0;
}

static void build_transport(void)
{
    CLAY(CLAY_ID("Transport"),
         { .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                   CLAY_SIZING_FIT(.min = 64) },
                       .padding = { 16, 16, 8, 8 },
                       .childGap = 16,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = SURFACE,
           .border = { .color = BORDER, .width = { .top = 1 } } })
    {
        CLAY_AUTO_ID(
            { .layout = { .sizing = { CLAY_SIZING_PERCENT(0.3f) },
                          .childGap = 10,
                          .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } })
        {
            if (app.has_now) {
                thumb_box(CLAY_ID("nowthumb"), app.now.id,
                          str_c(&app.now.thumbnail_url), 48, 30);
                CLAY_AUTO_ID(
                    { .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                                  .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                  .childGap = 1 } })
                {
                    text(cs(app.now.title.len ? str_c(&app.now.title)
                                              : app.now.id),
                         13, TEXT);
                    if (app.now.channel_title.len) {
                        text(cs(str_c(&app.now.channel_title)), 11, DIM);
                    }
                }
            }
        }
        CLAY_AUTO_ID(
            { .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                          .childGap = 12,
                          .childAlignment = { CLAY_ALIGN_X_CENTER,
                                              CLAY_ALIGN_Y_CENTER } } })
        {
            icon_button("restartBtn", CLAY_STRING("↺"), 16, 0, 0);
            Clay_String play_glyph = app.player.status == PS_LOADING ? spinner()
                                     : app.player.status == PS_PLAYING
                                         ? CLAY_STRING("II")
                                         : CLAY_STRING("▶");
            icon_button("playBtn", play_glyph, 18, 0, 1);
            icon_button("nextBtn", CLAY_STRING("▶▶"), 12, 0, 0);
            // until a real playback position arrives (cued, or still
            // loading after pressing play), hold the cursor at the resume point
            double pos = app.player.position, dur = app.player.duration;
            if (pos < 0.5) {
                pos = now_resume();
                if (dur <= 0 && app.now.duration_seconds > 0) {
                    dur = app.now.duration_seconds;
                }
            }
            char cur[16], tot[16];
            fmt_time(pos, cur, sizeof(cur));
            fmt_time(dur, tot, sizeof(tot));
            text(fstr("%s", cur), 11, FAINT);
            double frac = dur > 0 ? pos / dur : 0;
            if (frac > 1) {
                frac = 1;
            }
            CLAY(CLAY_ID("Seek"),
                 { .layout = { .sizing = { CLAY_SIZING_GROW(.max = 360),
                                           CLAY_SIZING_FIXED(6) },
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = SURFACE3,
                   .cornerRadius = CLAY_CORNER_RADIUS(3) })
            {
                CLAY_AUTO_ID(
                    { .layout = { .sizing = { CLAY_SIZING_PERCENT((float)frac),
                                              CLAY_SIZING_FIXED(6) } },
                      .backgroundColor = ACCENT,
                      .cornerRadius = CLAY_CORNER_RADIUS(3) })
                {
                }
            }
            text(fstr("%s", tot), 11, FAINT);
        }
        CLAY_AUTO_ID(
            { .layout = { .sizing = { CLAY_SIZING_PERCENT(0.18f) },
                          .childGap = 8,
                          .childAlignment = { .x = CLAY_ALIGN_X_RIGHT,
                                              .y = CLAY_ALIGN_Y_CENTER } } })
        {
            CLAY(CLAY_ID("Vol"),
                 { .layout = { .sizing = { CLAY_SIZING_FIXED(70),
                                           CLAY_SIZING_FIXED(6) } },
                   .backgroundColor = SURFACE3,
                   .cornerRadius = CLAY_CORNER_RADIUS(3) })
            {
                CLAY_AUTO_ID(
                    { .layout = { .sizing = { CLAY_SIZING_PERCENT(
                                                  (float)(app.player.volume /
                                                          100.0)),
                                              CLAY_SIZING_FIXED(6) } },
                      .backgroundColor = ACCENT,
                      .cornerRadius = CLAY_CORNER_RADIUS(3) })
                {
                }
            }
            icon_button("settingsBtn", CLAY_STRING("⚙"), 16, 0, 0);
        }
    }
}

static void build_dialog_field(const char *label, const char *content,
                               int focused, int multiline)
{
    text(cs(label), 13, TEXT);
    CLAY_AUTO_ID(
        { .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                  multiline ? CLAY_SIZING_FIXED(120)
                                            : CLAY_SIZING_FIT(.min = 38) },
                      .padding = CLAY_PADDING_ALL(10) },
          .backgroundColor = SURFACE2,
          .cornerRadius = CLAY_CORNER_RADIUS(3),
          .border = { .color = focused ? ACCENT : BORDER_STR,
                      .width = { 1, 1, 1, 1 } } })
    {
        Clay_String s = (content && content[0]) ? cs(content) : CLAY_STRING("");
        CLAY_TEXT(s, CLAY_TEXT_CONFIG(
                         { .fontSize = 13,
                           .fontId = 0,
                           .textColor = content && content[0] ? TEXT : FAINT,
                           .wrapMode = CLAY_TEXT_WRAP_WORDS }));
    }
}

static void build_dialog(Clay_String title, void (*body)(void))
{
    CLAY(CLAY_ID("Overlay"),
         { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .childAlignment = { CLAY_ALIGN_X_CENTER,
                                           CLAY_ALIGN_Y_CENTER } },
           .floating = { .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 10 },
           .backgroundColor = { 0, 0, 0, 140 } })
    {
        CLAY(CLAY_ID("Dialog"),
             { .layout = { .sizing = { CLAY_SIZING_FIXED(440) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = SURFACE,
               .cornerRadius = CLAY_CORNER_RADIUS(4),
               .border = { .color = BORDER, .width = { 1, 1, 1, 1 } } })
        {
            CLAY_AUTO_ID(
                { .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                              .padding = { 16, 12, 14, 14 },
                              .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                  .border = { .color = BORDER, .width = { .bottom = 1 } } })
            {
                text(title, 16, TEXT);
                CLAY_AUTO_ID(
                    { .layout = { .sizing = { CLAY_SIZING_GROW(0) } } })
                {
                }
                icon_button("dialogClose", CLAY_STRING("×"), 18, 0, 0);
            }
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                       .padding = CLAY_PADDING_ALL(16),
                                       .childGap = 8 } })
            {
                body();
            }
        }
    }
}

static void add_body(void)
{
    build_dialog_field("Paste YouTube URLs (one per line)", add_text,
                       focus == 1, 1);
    if (msg[0]) {
        text(cs(msg), 12, ACCENT_TEXT);
    }
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                               .childGap = 8,
                               .childAlignment = { .x = CLAY_ALIGN_X_RIGHT },
                               .padding = { 0, 0, 6, 0 } } })
    {
        CLAY(CLAY_ID("addSubmit"), { .layout = { .padding = { 16, 16, 9, 9 } },
                                     .backgroundColor = ACCENT,
                                     .cornerRadius = CLAY_CORNER_RADIUS(3) })
        {
            text(CLAY_STRING("Add to queue"), 13, WHITEC);
        }
    }
}

static void settings_body(void)
{
    build_dialog_field("YouTube Data API v3 key", key_text, focus == 2, 0);
    text(db_has_api_key(app.db)
             ? CLAY_STRING("A key is configured.")
             : CLAY_STRING("No key: titles via oEmbed only."),
         12, FAINT);
    if (msg[0]) {
        text(cs(msg), 12, ACCENT_TEXT);
    }
    char path[1024];
    home_join(TUNE_QUEUE_DEFAULT_DATA_DIR "/data.db", path, sizeof(path));
    text(fstr("DB: %s", path), 11, FAINT);
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                               .childGap = 8,
                               .childAlignment = { .x = CLAY_ALIGN_X_RIGHT },
                               .padding = { 0, 0, 6, 0 } } })
    {
        CLAY(CLAY_ID("keySave"), { .layout = { .padding = { 16, 16, 9, 9 } },
                                   .backgroundColor = ACCENT,
                                   .cornerRadius = CLAY_CORNER_RADIUS(3) })
        {
            text(CLAY_STRING("Save"), 13, WHITEC);
        }
    }
}

static void error_body(void)
{
    text(cs(error_msg[0] ? error_msg : "Could not load this track."), 13, TEXT);
    text(CLAY_STRING("Retry, or dismiss to skip it."), 12, FAINT);
    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) },
                               .childGap = 8,
                               .childAlignment = { .x = CLAY_ALIGN_X_RIGHT },
                               .padding = { 0, 0, 6, 0 } } })
    {
        CLAY(CLAY_ID("errDismiss"), { .layout = { .padding = { 16, 16, 9, 9 } },
                                      .backgroundColor = SURFACE3,
                                      .cornerRadius = CLAY_CORNER_RADIUS(3) })
        {
            text(CLAY_STRING("Dismiss"), 13, TEXT);
        }
        CLAY(CLAY_ID("errRetry"), { .layout = { .padding = { 16, 16, 9, 9 } },
                                    .backgroundColor = ACCENT,
                                    .cornerRadius = CLAY_CORNER_RADIUS(3) })
        {
            text(CLAY_STRING("Retry"), 13, WHITEC);
        }
    }
}

static void build_ui(void)
{
    CLAY(CLAY_ID("Root"),
         { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = BG })
    {
        CLAY(CLAY_ID("Main"), { .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                        CLAY_SIZING_GROW(0) },
                                            .padding = CLAY_PADDING_ALL(14),
                                            .childGap = 12 } })
        {
            build_stage();
            if (panel_open) {
                build_panel();
            }
        }
        build_transport();
    }
    if (add_open) {
        build_dialog(CLAY_STRING("Add to queue"), add_body);
    }
    if (settings_open) {
        build_dialog(CLAY_STRING("Settings"), settings_body);
    }
    if (error_open) {
        build_dialog(CLAY_STRING("Playback error"), error_body);
    }
}

// cover-crop a texture to fill the box, centered
static void draw_cover(Texture2D t, Clay_BoundingBox b)
{
    float tw = t.width, th = t.height;
    float scale = fmaxf(b.width / tw, b.height / th);
    float sw = b.width / scale, sh = b.height / scale;
    Rectangle src = { (tw - sw) / 2, (th - sh) / 2, sw, sh };
    Rectangle dst = { b.x, b.y, b.width, b.height };
    BeginScissorMode((int)b.x, (int)b.y, (int)b.width, (int)b.height);
    DrawTexturePro(t, src, dst, (Vector2){ 0, 0 }, 0, WHITE);
    EndScissorMode();
}

static void draw_stage_image(void)
{
    if (!app.has_now) {
        return;
    }
    Clay_ElementData d = Clay_GetElementData(id_of("Stage"));
    if (!d.found) {
        return;
    }
    Texture2D *fr = frames_get(app.now.id);
    if (fr && fr->id != 0) {
        draw_cover(*fr, d.boundingBox);
        return;
    }
    Texture2D *tx = thumbs_get(app.now.id, str_c(&app.now.thumbnail_url));
    if (tx && tx->id != 0) {
        draw_cover(*tx, d.boundingBox);
    }
}

static void handle_text_input(char *buf, int *len, int cap)
{
    int c;
    while ((c = GetCharPressed()) > 0) {
        if (c >= 32 && *len < cap - 1) {
            buf[(*len)++] = (char)c;
            buf[*len] = '\0';
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && *len > 0) {
        buf[--(*len)] = '\0';
    }
    if ((IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_LEFT_CONTROL)) &&
        IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip) {
            int n = (int)strlen(clip);
            if (*len + n < cap - 1) {
                memcpy(buf + *len, clip, n);
                *len += n;
                buf[*len] = '\0';
            }
        }
    }
    if (focus == 1 && IsKeyPressed(KEY_ENTER) && (IsKeyDown(KEY_LEFT_SHIFT))) {
        if (*len < cap - 1) {
            buf[(*len)++] = '\n';
            buf[*len] = '\0';
        }
    }
}

static void submit_add(void)
{
    const char **urls = NULL;
    int n = 0, cap = 0;
    char tmp[4096];
    strncpy(tmp, add_text, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *save, *tok = strtok_r(tmp, " \t\r\n", &save);
    while (tok) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            urls = realloc(urls, cap * sizeof(*urls));
        }
        urls[n++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &save);
    }
    struct add_result r = app_add_urls(&app, urls, n);
    free(urls);
    snprintf(msg, sizeof(msg), "Added %d, %d duplicate%s%s", r.added,
             r.duplicates, r.duplicates == 1 ? "" : "s",
             r.error_count ? ", some errors" : "");
    add_text[0] = '\0';
    add_len = 0;
}

// rows of the active view, unified so click handling has one shape
static int view_count(void)
{
    return view == VIEW_QUEUE   ? app.queue.count
           : view == VIEW_LIKED ? app.liked.count
                                : app.history.count;
}
static const struct video *view_row(int i, int *liked, struct queue_item **qi)
{
    *qi = NULL;
    if (view == VIEW_QUEUE) {
        *liked = app.queue.items[i].liked;
        *qi = &app.queue.items[i];
        return &app.queue.items[i].video;
    }
    if (view == VIEW_LIKED) {
        *liked = 1;
        return &app.liked.items[i].video;
    }
    *liked = app.history.items[i].liked;
    return &app.history.items[i].video;
}

// fraction 0..1 where a horizontal bar (seek/volume) was clicked, or -1 if not
// hit
static float bar_fraction(const char *id)
{
    if (!Clay_PointerOver(id_of(id))) {
        return -1;
    }
    Clay_ElementData d = Clay_GetElementData(id_of(id));
    if (!d.found) {
        return -1;
    }
    float frac = (GetMouseX() - d.boundingBox.x) / d.boundingBox.width;
    return frac < 0 ? 0 : frac > 1 ? 1 : frac;
}

static void handle_clicks(void)
{
    if (!g_clicked) {
        return;
    }

    if (error_open) {
        if (hit_s("dialogClose") || hit_s("errDismiss")) {
            error_open = 0;
        }
        if (hit_s("errRetry")) {
            error_open = 0;
            app_play_now(&app);
        }
        return;
    }
    if (add_open || settings_open) {
        if (hit_s("dialogClose")) {
            add_open = settings_open = 0;
            focus = 0;
            msg[0] = 0;
        }
        if (add_open) {
            if (Clay_PointerOver(id_of("Dialog"))) {
                focus = 1;
            }
            if (hit_s("addSubmit")) {
                submit_add();
            }
        }
        if (settings_open) {
            if (Clay_PointerOver(id_of("Dialog"))) {
                focus = 2;
            }
            if (hit_s("keySave")) {
                char err[256] = "";
                if (app_set_api_key(&app, key_text, err, sizeof(err)) == 0) {
                    snprintf(msg, sizeof(msg), "Saved.");
                } else {
                    snprintf(msg, sizeof(msg), "%s", err);
                }
            }
        }
        return;
    }
    if (hit_s("tabQueue")) {
        view = VIEW_QUEUE;
    }
    if (hit_s("tabLiked")) {
        view = VIEW_LIKED;
    }
    if (hit_s("tabHistory")) {
        view = VIEW_HISTORY;
    }
    if (hit_s("addBtn")) {
        add_open = 1;
        focus = 1;
        msg[0] = 0;
    }
    if (hit_s("settingsBtn")) {
        settings_open = 1;
        focus = 0;
        msg[0] = 0;
        key_text[0] = 0;
        key_len = 0;
    }
    if (hit_s("playBtn")) {
        app_press_play(&app);
    }
    if (hit_s("nextBtn")) {
        if (app.has_now) {
            app_advance(&app, "skipped");
        }
    }
    if (hit_s("restartBtn")) {
        player_seek(&app.player, 0);
    }

    float seek = bar_fraction("Seek");
    if (seek >= 0 && app.player.duration > 0) {
        player_seek(&app.player, seek * app.player.duration);
    }
    float vol = bar_fraction("Vol");
    if (vol >= 0) {
        player_set_volume(&app.player, vol * 100);
    }

    for (int i = 0; i < view_count(); i++) {
        int liked;
        struct queue_item *qi;
        const struct video *v = view_row(i, &liked, &qi);
        if (hit(idi("like", i))) {
            app_set_like(&app, v->id, !liked);
        } else if (hit(idi("play", i)) || hit(idi("row", i))) {
            if (qi) {
                app_play_queue_item(&app, qi);
            } else {
                app_play_video(&app, v, 0);
            }
        }
    }
}

static struct tune_queue_app_config build_config(void)
{
    static char db_file[1024], cache[800];
    home_join(TUNE_QUEUE_DEFAULT_DATA_DIR "/data.db", db_file, sizeof(db_file));
    home_join(TUNE_QUEUE_DEFAULT_CACHE_DIR, cache, sizeof(cache));
    return (struct tune_queue_app_config){
        .core = {
            .db_path = db_file,
            .ytdl_path = "yt-dlp",
            .heartbeat_interval = TUNE_QUEUE_DEFAULT_HEARTBEAT_INTERVAL,
        },
        .gui = {
            .mpv_path = "mpv",
            .gui_font_path = FONT_PATH,
            .cache_dir = cache,
        },
    };
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT |
                   FLAG_WINDOW_HIGHDPI);
    InitWindow(1100, 720, "TuneQueue");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    InitOverlay();

    struct tune_queue_app_config cfg = build_config();
    font_path = cfg.gui.gui_font_path;
    app_init(&app, &cfg);
    thumbs_init(cfg.gui.cache_dir);
    frames_init(cfg.gui.cache_dir, cfg.gui.mpv_path, cfg.core.ytdl_path);

    uint64_t memsize = Clay_MinMemorySize();
    Clay_Arena arena =
        Clay_CreateArenaWithCapacityAndMemory(memsize, malloc(memsize));
    Clay_Initialize(arena,
                    (Clay_Dimensions){ GetScreenWidth(), GetScreenHeight() },
                    (Clay_ErrorHandler){ 0 });
    Clay_SetMeasureTextFunction(measure, fonts);

    ensure_font();

    char shown_id[16] = "";
    while (!WindowShouldClose()) {
        if (app_take_dirty(&app)) {
            gui_reload();
        }

        float dt = GetFrameTime();
        enum tune_queue_app_event ev = app_poll(&app, dt);

        // reset the still timer whenever the track changes (manual play,
        // auto-advance, or retry all route through the core)
        const char *now_id = app.has_now ? app.now.id : "";
        if (strcmp(shown_id, now_id) != 0) {
            snprintf(shown_id, sizeof(shown_id), "%s", now_id);
            frames_due = FRAME_FIRST;
        }
        if (app.player.status == PS_PLAYING) {
            frames_due -= dt;
            if (frames_due <= 0) {
                frames_request(app.now.id, app.player.position);
                frames_due = FRAME_INTERVAL;
            }
        }
        if (ev == TUNE_QUEUE_APP_TRACK_FAILED && !error_open) {
            snprintf(error_msg, sizeof(error_msg), "Could not load “%s”.",
                     app.now.title.len ? str_c(&app.now.title) : app.now.id);
            error_open = 1;
        }

        if (focus == 1) {
            handle_text_input(add_text, &add_len, sizeof(add_text));
        } else if (focus == 2) {
            handle_text_input(key_text, &key_len, sizeof(key_text));
        }
        if (!focus) {
            if (IsKeyPressed(KEY_SPACE)) {
                app_press_play(&app);
            }
            if (IsKeyPressed(KEY_RIGHT) && app.has_now) {
                app_advance(&app, "skipped");
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            add_open = settings_open = error_open = 0;
            focus = 0;
        }
        if ((IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) &&
            IsKeyPressed(KEY_Q)) {
            break;
        }

        Vector2 m = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ m.x, m.y },
                             IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(
            true,
            (Clay_Vector2){ GetMouseWheelMoveV().x * 6,
                            GetMouseWheelMoveV().y * 6 },
            dt);
        Clay_SetLayoutDimensions(
            (Clay_Dimensions){ GetScreenWidth(), GetScreenHeight() });

        fpool_reset();
        Clay_BeginLayout();
        build_ui();
        Clay_RenderCommandArray cmds = Clay_EndLayout(dt);

        g_clicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        handle_clicks();

        thumbs_pump();
        frames_pump();
        BeginDrawing();
        ClearBackground((Color){ 18, 18, 17, 255 });
        Clay_Raylib_Render(cmds, fonts);
        // drawn over Clay's output, so hold it back under an open modal
        if (!add_open && !settings_open && !error_open) {
            draw_stage_image();
        }
        EndDrawing();
    }

    thumbs_shutdown();
    frames_shutdown();
    app_shutdown(&app);
    CloseWindow();
    return 0;
}
