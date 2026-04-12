#include "app.h"

#include "../core/base.h"
#include "../platform/catalog.h"
#include "../platform/catalog_aliases.h"
#include "../platform/catalog_watch.h"
#include "../platform/everything_client.h"
#include "../platform/icon_worker.h"
#include "../platform/launch.h"
#include "../render/dx11_renderer.h"
#include "../search/fuzzy.h"
#include "../text/kb_text.h"
#include "../ui/ui.h"
#include "../ui/ctx_menu_icons.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "Shcore.lib")

#define LAUNCHER_RESULTS_FONT_PX 18.0f
#define LAUNCHER_QUERY_FONT_PX 20.0f
#define LAUNCHER_CTX_MENU_FONT_PX 14.5f
#define LAUNCHER_UI_FONT_PX 24.0f
#define LAUNCHER_BASE_WINDOW_W 920
#define LAUNCHER_BASE_WINDOW_H 560
#define LAUNCHER_DEFAULT_VISIBLE_ROWS 10
#define LAUNCHER_MIN_VISIBLE_ROWS 1
#define LAUNCHER_MAX_VISIBLE_ROWS 50
#define LAUNCHER_ICON_SIZE_PX 24
#define LAUNCHER_ICON_CACHE_CAPACITY 512
#define LAUNCHER_ICON_QUEUE_CAPACITY 1024
#define LAUNCHER_CTX_FILTER_CAP 128
#define IDT_CATALOG_FS_DEBOUNCE 3

static const f32 k_debug_text_gamma_presets[] = {
    1.0f,
    1.6f,
    1.8f,
    2.2f,
    2.4f,
    2.8f,
};
static const u32 k_debug_text_gamma_preset_count = sizeof(k_debug_text_gamma_presets) / sizeof(k_debug_text_gamma_presets[0]);
static u32 s_debug_text_gamma_preset_index = 2;

static const f32 k_debug_text_gamma_blend_presets[] = {
    1.0f,
    0.95f,
    0.88f,
    0.82f,
    0.75f,
};
static const u32 k_debug_text_gamma_blend_preset_count = sizeof(k_debug_text_gamma_blend_presets) / sizeof(k_debug_text_gamma_blend_presets[0]);
static u32 s_debug_text_gamma_blend_index = 2;

static const char *k_text_render_mode_names[] = {
    "legacy",
    "raw",
    "gammaA",
    "linBg",
};

typedef enum AppIconState {
    AppIconState_Missing = 0,
    AppIconState_Queued = 1,
    AppIconState_Ready = 2,
    AppIconState_Failed = 3,
} AppIconState;

typedef struct AppIconEntry {
    wchar_t *path;
    s32 icon_index;
    u32 generation;
    AppIconState state;
    Dx11Texture texture;
    u32 last_used_frame;
} AppIconEntry;

typedef struct AppListGeometry {
    UiRect list_content_rect;
    f32 row_step;
    f32 first_row_y;
    f32 content_right;
    s32 start_index;
    s32 rows_per_page;
} AppListGeometry;

typedef struct AppState {
    HWND hwnd;
    Arena permanent_arena;
    Arena catalog_arena;
    Arena results_arena;
    Arena frame_arena;
    Dx11Renderer renderer;
    KbTextSystem text;
    KbTextSystem text_results;
    KbTextSystem text_query;
    KbTextSystem text_ctx_menu;
    LaunchItemArray app_catalog;
    CatalogAliases catalog_aliases;
    SearchResultArray results;
    SearchMode mode;
    bool visible;
    bool everything_available;
    s32 selected_index;
    s32 hover_result_index;
    s32 hover_lnk_chip_row;
    s32 results_top_index;
    s32 max_visible_rows;
    s32 query_length;
    s32 query_caret;
    s32 query_anchor;
    f32 query_scroll_x;
    u32 caret_blink_tick_ms;
    bool caret_blink_on;
    wchar_t install_dir[MAX_PATH * 4];
    char query[LAUNCHER_MAX_QUERY];
    AppIconEntry icon_cache[LAUNCHER_ICON_CACHE_CAPACITY];
    s32 icon_cache_count;
    IconWorker icon_worker;
    u32 frame_counter;
    u32 dpi;
    f32 dpi_scale;
    bool ctx_open;
    s32 ctx_target_index;
    f32 ctx_anchor_x;
    f32 ctx_anchor_y;
    char ctx_filter[LAUNCHER_CTX_FILTER_CAP];
    s32 ctx_filter_len;
    s32 ctx_filter_caret;
    s32 ctx_filter_anchor;
    f32 ctx_filter_scroll_x;
    s32 ctx_selected;
    s32 ctx_hover_row;
    UiRect ctx_panel_rect;
    UiRect ctx_filter_bar_rect;
    UiRect ctx_filter_text_rect;
    f32 ctx_list_row0_y;
    f32 ctx_row_h;
    s32 ctx_filtered_count;
    Dx11Texture ctx_menu_icons[CTX_MENU_ICON_COUNT];
} AppState;

static AppState *g_app = NULL;

static void app_refresh_results(AppState *app);
static s32 app_result_rows_per_page(AppState *app);
static f32 app_results_row_step(AppState *app);
static void app_scroll_results_list(AppState *app, s32 wheel_delta);
static s32 app_hit_test_result_index(AppState *app, f32 mx, f32 my, u32 client_w, u32 client_h);
static s32 app_hit_test_lnk_badge_row(AppState *app, f32 mx, f32 my, u32 client_w, u32 client_h);
static void app_clamp_result_view(AppState *app);
static void app_clamp_results_top_bounds(AppState *app);
static void app_window_size_for_rows(AppState *app, s32 *out_width, s32 *out_height);
static s32 app_find_icon_entry(AppState *app, const wchar_t *path, s32 icon_index);
static s32 app_get_or_create_icon_entry(AppState *app, const wchar_t *path, s32 icon_index);
static void app_request_item_icon(AppState *app, const LaunchItem *item);
static void app_process_icon_completions(AppState *app, s32 budget);
static void app_shutdown_icon_cache(AppState *app);
static u32 app_primary_monitor_dpi(void);
static bool app_load_text_systems(AppState *app, const wchar_t *font_path);
static void app_unload_text_systems(AppState *app);
static void app_invalidate_icon_textures(AppState *app);
static void app_hide(AppState *app);

static s32
app_clamp_query_index(AppState *app, s32 index)
{
    if (index < 0) {
        return 0;
    }
    if (index > app->query_length) {
        return app->query_length;
    }
    return index;
}

static bool
app_query_has_selection(AppState *app)
{
    return app->query_anchor != app->query_caret;
}

static void
app_query_selection_bounds(AppState *app, s32 *out_start, s32 *out_end)
{
    s32 a = app->query_anchor;
    s32 b = app->query_caret;
    if (a <= b) {
        *out_start = a;
        *out_end = b;
    } else {
        *out_start = b;
        *out_end = a;
    }
}

static void
app_query_set_caret(AppState *app, s32 new_caret, bool extend_selection)
{
    app->query_caret = app_clamp_query_index(app, new_caret);
    if (!extend_selection) {
        app->query_anchor = app->query_caret;
    }
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
}

static bool
app_query_delete_range(AppState *app, s32 start, s32 end)
{
    start = app_clamp_query_index(app, start);
    end = app_clamp_query_index(app, end);
    if (end <= start) {
        return false;
    }

    memmove(app->query + start, app->query + end, (size_t)(app->query_length - end + 1));
    app->query_length -= (end - start);
    app->query_caret = start;
    app->query_anchor = start;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
    return true;
}

static bool
app_query_delete_selection(AppState *app)
{
    if (!app_query_has_selection(app)) {
        return false;
    }
    s32 start = 0;
    s32 end = 0;
    app_query_selection_bounds(app, &start, &end);
    return app_query_delete_range(app, start, end);
}

static bool
app_query_insert_char(AppState *app, char c)
{
    if (app_query_has_selection(app)) {
        app_query_delete_selection(app);
    }
    if (app->query_length + 1 >= (s32)array_count(app->query)) {
        return false;
    }

    memmove(app->query + app->query_caret + 1,
            app->query + app->query_caret,
            (size_t)(app->query_length - app->query_caret + 1));
    app->query[app->query_caret] = c;
    app->query_length += 1;
    app->query_caret += 1;
    app->query_anchor = app->query_caret;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
    return true;
}

static bool
app_query_is_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static s32
app_query_word_left(AppState *app, s32 from)
{
    s32 i = app_clamp_query_index(app, from);
    while (i > 0 && !app_query_is_word_char(app->query[i - 1])) {
        --i;
    }
    while (i > 0 && app_query_is_word_char(app->query[i - 1])) {
        --i;
    }
    return i;
}

static s32
app_query_word_right(AppState *app, s32 from)
{
    s32 i = app_clamp_query_index(app, from);
    while (i < app->query_length && !app_query_is_word_char(app->query[i])) {
        ++i;
    }
    while (i < app->query_length && app_query_is_word_char(app->query[i])) {
        ++i;
    }
    return i;
}

static size_t
utf8_clamped_byte_len(const char *utf8, size_t max_bytes)
{
    size_t i = 0;
    while (i < max_bytes && utf8[i]) {
        unsigned char c = (unsigned char)utf8[i];
        size_t n = 1;
        if ((c & 0x80) == 0) {
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        } else {
            ++i;
            continue;
        }
        if (i + n > max_bytes) {
            break;
        }
        i += n;
    }
    return i;
}

/* Byte index after the UTF-8 codepoint that starts at `off` (invalid bytes advance by 1). */
static size_t
utf8_next_cp_byte(const char *s, size_t len, size_t off)
{
    if (off >= len) {
        return len;
    }
    unsigned char c = (unsigned char)s[off];
    size_t nb = 1;
    if (c < 0x80u) {
        nb = 1;
    } else if ((c & 0xE0u) == 0xC0u) {
        nb = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
        nb = 3;
    } else if ((c & 0xF8u) == 0xF0u) {
        nb = 4;
    }
    if (off + nb > len) {
        return len;
    }
    return off + nb;
}

/* starts[k] = byte offset after k UTF-8 codepoints; starts[0]=0; *out_nchars = n with starts[n]==len. */
static void
utf8_prefix_byte_offsets(const char *s, size_t len, size_t *starts, size_t *out_nchars, size_t max_starts)
{
    starts[0] = 0;
    size_t n = 0;
    size_t i = 0;
    while (i < len && n + 1 < max_starts) {
        i = utf8_next_cp_byte(s, len, i);
        n++;
        starts[n] = i;
    }
    *out_nchars = n;
}

static bool
app_clipboard_set_utf8(HWND hwnd, const char *utf8, s32 byte_len)
{
    if (!utf8) {
        utf8 = "";
    }
    if (byte_len < 0) {
        byte_len = (s32)strlen(utf8);
    }

    int wide_count = 0;
    if (byte_len > 0) {
        wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, byte_len, NULL, 0);
        if (wide_count <= 0) {
            wide_count = MultiByteToWideChar(CP_UTF8, 0, utf8, byte_len, NULL, 0);
        }
    }
    if (wide_count < 0) {
        wide_count = 0;
    }

    SIZE_T alloc_bytes = ((SIZE_T)wide_count + 1) * sizeof(wchar_t);
    HGLOBAL h_mem = GlobalAlloc(GMEM_MOVEABLE, alloc_bytes);
    if (!h_mem) {
        return false;
    }
    wchar_t *dst = (wchar_t *)GlobalLock(h_mem);
    if (!dst) {
        GlobalFree(h_mem);
        return false;
    }
    if (wide_count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, byte_len, dst, wide_count);
    }
    dst[wide_count] = 0;
    GlobalUnlock(h_mem);

    if (!OpenClipboard(hwnd)) {
        GlobalFree(h_mem);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h_mem)) {
        GlobalFree(h_mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

static bool
app_clipboard_get_utf8(HWND hwnd, Arena *arena, const char **out_utf8)
{
    if (!out_utf8) {
        return false;
    }
    *out_utf8 = NULL;
    if (!OpenClipboard(hwnd)) {
        return false;
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        CloseClipboard();
        return false;
    }
    wchar_t *w = (wchar_t *)GlobalLock(h);
    if (!w) {
        CloseClipboard();
        return false;
    }
    char *utf8 = utf8_from_wide(arena, w);
    GlobalUnlock(h);
    CloseClipboard();
    *out_utf8 = utf8;
    return true;
}

static bool
app_query_insert_utf8(AppState *app, const char *utf8)
{
    if (!utf8) {
        return false;
    }
    size_t ins_len = strlen(utf8);
    if (ins_len == 0) {
        return true;
    }

    if (app_query_has_selection(app)) {
        app_query_delete_selection(app);
    }

    s32 room = LAUNCHER_MAX_QUERY - 1 - app->query_length;
    if (room <= 0) {
        return false;
    }

    if ((s32)ins_len > room) {
        ins_len = utf8_clamped_byte_len(utf8, (size_t)room);
    }
    if (ins_len == 0) {
        return false;
    }

    memmove(app->query + app->query_caret + ins_len,
            app->query + app->query_caret,
            (size_t)(app->query_length - app->query_caret + 1));
    memcpy(app->query + app->query_caret, utf8, ins_len);
    app->query_length += (s32)ins_len;
    app->query_caret += (s32)ins_len;
    app->query_anchor = app->query_caret;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
    return true;
}

static bool
app_query_paste_from_clipboard(AppState *app)
{
    const char *text = NULL;
    if (!app_clipboard_get_utf8(app->hwnd, &app->frame_arena, &text)) {
        return false;
    }
    if (!text) {
        return true;
    }
    return app_query_insert_utf8(app, text);
}

static bool
app_query_copy_to_clipboard(AppState *app)
{
    if (!app_query_has_selection(app)) {
        return false;
    }
    s32 start = 0;
    s32 end = 0;
    app_query_selection_bounds(app, &start, &end);
    return app_clipboard_set_utf8(app->hwnd, app->query + start, end - start);
}

static bool
app_query_cut_to_clipboard(AppState *app)
{
    if (!app_query_has_selection(app)) {
        return false;
    }
    s32 start = 0;
    s32 end = 0;
    app_query_selection_bounds(app, &start, &end);
    if (!app_clipboard_set_utf8(app->hwnd, app->query + start, end - start)) {
        return false;
    }
    return app_query_delete_selection(app);
}

typedef enum CtxAction {
    CtxAction_Launch = 0,
    CtxAction_CopyPath = 1,
    CtxAction_OpenLocation = 2,
} CtxAction;

typedef struct CtxMenuPick {
    u8 action;
    double score;
} CtxMenuPick;

static const char *k_ctx_menu_labels[3] = {
    "Launch",
    "Copy path",
    "Open file location",
};

static const char *k_ctx_menu_fuzzy[3] = {
    "launch",
    "copy path",
    "open file location",
};

static int
ctx_menu_pick_compare(const void *a, const void *b)
{
    const CtxMenuPick *x = (const CtxMenuPick *)a;
    const CtxMenuPick *y = (const CtxMenuPick *)b;
    if (x->score > y->score) {
        return -1;
    }
    if (x->score < y->score) {
        return 1;
    }
    return (int)x->action - (int)y->action;
}

static s32
ctx_menu_build_picks(const char *filter_lower, CtxMenuPick *out)
{
    bool filtered = (filter_lower && filter_lower[0]);
    s32 n = 0;
    for (u32 i = 0; i < 3; ++i) {
        if (filtered) {
            FuzzyMatch m = fuzzy_score_text(filter_lower, k_ctx_menu_fuzzy[i]);
            if (!m.matched) {
                continue;
            }
            out[n].action = (u8)i;
            out[n].score = m.score;
        } else {
            out[n].action = (u8)i;
            out[n].score = 1.0 - (double)i * 0.001;
        }
        n++;
    }
    if (filtered && n > 1) {
        qsort(out, (size_t)n, sizeof(CtxMenuPick), ctx_menu_pick_compare);
    }
    return n;
}

static void
app_ctx_menu_close(AppState *app)
{
    if (!app) {
        return;
    }
    app->ctx_open = false;
    app->ctx_hover_row = -1;
    app->ctx_filter[0] = 0;
    app->ctx_filter_len = 0;
    app->ctx_filter_caret = 0;
    app->ctx_filter_anchor = 0;
    app->ctx_filter_scroll_x = 0.0f;
    app->ctx_filtered_count = 0;
}

static s32
app_ctx_clamp_filter_index(AppState *app, s32 index)
{
    if (index < 0) {
        return 0;
    }
    if (index > app->ctx_filter_len) {
        return app->ctx_filter_len;
    }
    return index;
}

static bool
app_ctx_filter_has_selection(AppState *app)
{
    return app->ctx_filter_anchor != app->ctx_filter_caret;
}

static void
app_ctx_filter_selection_bounds(AppState *app, s32 *out_start, s32 *out_end)
{
    s32 a = app->ctx_filter_anchor;
    s32 b = app->ctx_filter_caret;
    if (a <= b) {
        *out_start = a;
        *out_end = b;
    } else {
        *out_start = b;
        *out_end = a;
    }
}

static void
app_ctx_filter_set_caret(AppState *app, s32 new_caret, bool extend_selection)
{
    app->ctx_filter_caret = app_ctx_clamp_filter_index(app, new_caret);
    if (!extend_selection) {
        app->ctx_filter_anchor = app->ctx_filter_caret;
    }
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
}

static bool
app_ctx_filter_delete_range(AppState *app, s32 start, s32 end)
{
    start = app_ctx_clamp_filter_index(app, start);
    end = app_ctx_clamp_filter_index(app, end);
    if (end <= start) {
        return false;
    }
    memmove(app->ctx_filter + start, app->ctx_filter + end, (size_t)(app->ctx_filter_len - end + 1));
    app->ctx_filter_len -= (end - start);
    app->ctx_filter_caret = start;
    app->ctx_filter_anchor = start;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
    return true;
}

static bool
app_ctx_filter_delete_selection(AppState *app)
{
    if (!app_ctx_filter_has_selection(app)) {
        return false;
    }
    s32 start = 0;
    s32 end = 0;
    app_ctx_filter_selection_bounds(app, &start, &end);
    return app_ctx_filter_delete_range(app, start, end);
}

static bool
app_ctx_filter_insert_utf8(AppState *app, const char *utf8)
{
    if (!utf8) {
        return false;
    }
    size_t ins_len = strlen(utf8);
    if (ins_len == 0) {
        return true;
    }
    if (app_ctx_filter_has_selection(app)) {
        app_ctx_filter_delete_selection(app);
    }
    s32 room = LAUNCHER_CTX_FILTER_CAP - 1 - app->ctx_filter_len;
    if (room <= 0) {
        return false;
    }
    if ((s32)ins_len > room) {
        ins_len = utf8_clamped_byte_len(utf8, (size_t)room);
    }
    if (ins_len == 0) {
        return false;
    }
    memmove(app->ctx_filter + app->ctx_filter_caret + ins_len,
            app->ctx_filter + app->ctx_filter_caret,
            (size_t)(app->ctx_filter_len - app->ctx_filter_caret + 1));
    memcpy(app->ctx_filter + app->ctx_filter_caret, utf8, ins_len);
    app->ctx_filter_len += (s32)ins_len;
    app->ctx_filter_caret += (s32)ins_len;
    app->ctx_filter_anchor = app->ctx_filter_caret;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
    return true;
}

static bool
app_ctx_filter_insert_char(AppState *app, char c)
{
    if (app_ctx_filter_has_selection(app)) {
        app_ctx_filter_delete_selection(app);
    }
    if (app->ctx_filter_len + 1 >= LAUNCHER_CTX_FILTER_CAP) {
        return false;
    }
    memmove(app->ctx_filter + app->ctx_filter_caret + 1,
            app->ctx_filter + app->ctx_filter_caret,
            (size_t)(app->ctx_filter_len - app->ctx_filter_caret + 1));
    app->ctx_filter[app->ctx_filter_caret] = c;
    app->ctx_filter_len += 1;
    app->ctx_filter_caret += 1;
    app->ctx_filter_anchor = app->ctx_filter_caret;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
    return true;
}

static s32
app_ctx_filter_word_left(AppState *app, s32 from)
{
    s32 i = app_ctx_clamp_filter_index(app, from);
    while (i > 0 && !app_query_is_word_char(app->ctx_filter[i - 1])) {
        --i;
    }
    while (i > 0 && app_query_is_word_char(app->ctx_filter[i - 1])) {
        --i;
    }
    return i;
}

static s32
app_ctx_filter_word_right(AppState *app, s32 from)
{
    s32 i = app_ctx_clamp_filter_index(app, from);
    while (i < app->ctx_filter_len && !app_query_is_word_char(app->ctx_filter[i])) {
        ++i;
    }
    while (i < app->ctx_filter_len && app_query_is_word_char(app->ctx_filter[i])) {
        ++i;
    }
    return i;
}

static bool
app_ctx_filter_paste_from_clipboard(AppState *app)
{
    const char *text = NULL;
    if (!app_clipboard_get_utf8(app->hwnd, &app->frame_arena, &text)) {
        return false;
    }
    if (!text) {
        return true;
    }
    return app_ctx_filter_insert_utf8(app, text);
}

static bool
app_ctx_filter_copy_to_clipboard(AppState *app)
{
    if (!app_ctx_filter_has_selection(app)) {
        return false;
    }
    s32 start = 0;
    s32 end = 0;
    app_ctx_filter_selection_bounds(app, &start, &end);
    return app_clipboard_set_utf8(app->hwnd, app->ctx_filter + start, end - start);
}

static bool
app_ctx_filter_cut_to_clipboard(AppState *app)
{
    if (!app_ctx_filter_has_selection(app)) {
        return false;
    }
    s32 start = 0;
    s32 end = 0;
    app_ctx_filter_selection_bounds(app, &start, &end);
    if (!app_clipboard_set_utf8(app->hwnd, app->ctx_filter + start, end - start)) {
        return false;
    }
    return app_ctx_filter_delete_selection(app);
}

static void
app_ctx_clamp_menu_selection(AppState *app, s32 filtered_count)
{
    if (filtered_count <= 0) {
        app->ctx_selected = 0;
        return;
    }
    if (app->ctx_selected < 0) {
        app->ctx_selected = 0;
    }
    if (app->ctx_selected >= filtered_count) {
        app->ctx_selected = filtered_count - 1;
    }
}

static s32
app_ctx_rebuild_picks(AppState *app, CtxMenuPick *picks)
{
    char lower[LAUNCHER_CTX_FILTER_CAP];
    strcpy_s(lower, sizeof(lower), app->ctx_filter);
    lowercase_ascii_in_place(lower);
    return ctx_menu_build_picks(lower, picks);
}

static void
app_ctx_menu_update_layout(AppState *app, u32 client_w, u32 client_h, const CtxMenuPick *picks, s32 n)
{
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    f32 pad = 3.0f * s;
    /* Must match context menu border thickness in app_render (1.0f * s). */
    f32 ctx_border = 1.0f * s;
    f32 filter_h = 22.0f * s;
    f32 row_h = app->text_ctx_menu.line_height + 4.0f * s;
    f32 min_row = 22.0f * s;
    if (row_h < min_row) {
        row_h = min_row;
    }

    /* Horizontal slack must match ctx row insets: ctx_pad*2 + inner pad*2 + [icon + gap]. */
    f32 ctx_row_pad = 8.0f * s;
    f32 row_inner_pad = 2.0f * s;
    f32 icon_row_extra = (18.0f * s) + (7.0f * s); /* max icon side + icon_gap in ui_control_context_menu_item */
    f32 min_w = 185.0f * s;
    f32 label_w = min_w;
    ArenaTemp temp = arena_temp_begin(&app->frame_arena);
    for (s32 i = 0; i < n; ++i) {
        f32 w = kb_text_measure_utf8_width(&app->frame_arena, &app->text_ctx_menu, k_ctx_menu_labels[picks[i].action]);
        f32 h_slack = 2.0f * ctx_row_pad + 2.0f * row_inner_pad;
        if (picks[i].action != (u8)CtxAction_Launch) {
            h_slack += icon_row_extra;
        }
        w += h_slack;
        if (w > label_w) {
            label_w = w;
        }
    }
    arena_temp_end(temp);

    f32 panel_w = label_w;
    f32 list_h = (f32)n * row_h;
    f32 filter_list_gap = 4.0f * s;
    f32 pad_bottom_extra = 5.0f * s;
    f32 panel_h = ctx_border + filter_h + filter_list_gap + list_h + pad + pad_bottom_extra + ctx_border;
    if (n == 0) {
        panel_h = ctx_border + filter_h + pad + pad_bottom_extra + ctx_border;
    }

    f32 x = app->ctx_anchor_x;
    f32 y = app->ctx_anchor_y;
    f32 max_x = (f32)client_w - 4.0f * s - panel_w;
    f32 max_y = (f32)client_h - 4.0f * s - panel_h;
    if (x > max_x) {
        x = max_x;
    }
    if (y > max_y) {
        y = max_y;
    }
    if (x < 4.0f * s) {
        x = 4.0f * s;
    }
    if (y < 4.0f * s) {
        y = 4.0f * s;
    }

    app->ctx_panel_rect = ui_rect(x, y, panel_w, panel_h);
    /* Filter strip: full width/top inside the menu border (no top_bar margin on L/T/R). */
    app->ctx_filter_bar_rect = ui_rect(x + ctx_border, y + ctx_border, panel_w - ctx_border * 2.0f, filter_h);
    f32 filter_text_pad_x = 6.0f * s;
    f32 filter_text_pad_y = 2.0f * s;
    app->ctx_filter_text_rect =
        ui_inset(app->ctx_filter_bar_rect, filter_text_pad_x, filter_text_pad_y, filter_text_pad_x, filter_text_pad_y);
    app->ctx_list_row0_y = y + ctx_border + filter_h + (n > 0 ? filter_list_gap : 0.0f);
    app->ctx_row_h = row_h;
    app->ctx_filtered_count = n;
}

/* Returns: -2 outside panel, -1 filter row, >= 0 filtered item index */
static s32
app_ctx_hit_test(AppState *app, f32 mx, f32 my)
{
    UiRect p = app->ctx_panel_rect;
    if (mx < p.x || my < p.y || mx >= p.x + p.w || my >= p.y + p.h) {
        return -2;
    }
    UiRect f = app->ctx_filter_bar_rect;
    if (mx >= f.x && my >= f.y && mx < f.x + f.w && my < f.y + f.h) {
        return -1;
    }
    if (app->ctx_filtered_count <= 0) {
        return -2;
    }
    f32 ry = my - app->ctx_list_row0_y;
    if (ry < 0.0f) {
        return -2;
    }
    s32 slot = (s32)floorf(ry / app->ctx_row_h);
    if (slot < 0 || slot >= app->ctx_filtered_count) {
        return -2;
    }
    return slot;
}

static bool
app_ctx_item_enabled(const LaunchItem *item, CtxAction action)
{
    (void)action;
    return item && item->launch_path && item->launch_path[0];
}

static void
app_ctx_run_action(AppState *app, CtxAction action, bool elevated_launch)
{
    if (!app->ctx_open || app->ctx_target_index < 0 || (u32)app->ctx_target_index >= app->results.count) {
        return;
    }
    const LaunchItem *item = app->results.items[(u32)app->ctx_target_index].item;
    if (!app_ctx_item_enabled(item, action)) {
        return;
    }
    switch (action) {
    case CtxAction_Launch:
        (void)platform_launch_item(item, elevated_launch);
        break;
    case CtxAction_CopyPath: {
        char *utf8 = utf8_from_wide(&app->frame_arena, item->launch_path);
        (void)app_clipboard_set_utf8(app->hwnd, utf8, -1);
    } break;
    case CtxAction_OpenLocation:
        (void)platform_open_file_location(item);
        break;
    default:
        break;
    }
    app_hide(app);
}

static void
app_ctx_activate_selection(AppState *app, bool elevated_launch)
{
    CtxMenuPick picks[3];
    s32 n = app_ctx_rebuild_picks(app, picks);
    app_ctx_clamp_menu_selection(app, n);
    if (n <= 0) {
        return;
    }
    app_ctx_run_action(app, (CtxAction)picks[app->ctx_selected].action, elevated_launch);
}

static void
app_ctx_menu_open_at(AppState *app, s32 result_index, f32 anchor_x, f32 anchor_y)
{
    if (result_index < 0 || (u32)result_index >= app->results.count) {
        return;
    }
    app_ctx_menu_close(app);
    app->ctx_open = true;
    app->ctx_target_index = result_index;
    app->ctx_anchor_x = anchor_x;
    app->ctx_anchor_y = anchor_y;
    app->ctx_selected = 0;
    app->ctx_hover_row = -1;
    app->caret_blink_on = true;
    app->caret_blink_tick_ms = GetTickCount();
}

static bool
app_ctx_menu_keydown(AppState *app, HWND hwnd, WPARAM wParam, bool ctrl_down, bool shift_down)
{
    bool changed = false;
    CtxMenuPick picks[3];
    s32 n = app_ctx_rebuild_picks(app, picks);

    switch (wParam) {
    case VK_ESCAPE:
        app_ctx_menu_close(app);
        changed = true;
        break;
    case VK_RETURN:
        app_ctx_activate_selection(app, ctrl_down && shift_down);
        changed = true;
        break;
    case VK_UP:
        app_ctx_clamp_menu_selection(app, n);
        if (n > 0 && app->ctx_selected > 0) {
            app->ctx_selected--;
            changed = true;
        }
        break;
    case VK_DOWN:
        app_ctx_clamp_menu_selection(app, n);
        if (n > 0 && app->ctx_selected < n - 1) {
            app->ctx_selected++;
            changed = true;
        }
        break;
    case VK_TAB:
        changed = true;
        break;
    case 'A':
        if (ctrl_down) {
            app->ctx_filter_anchor = 0;
            app_ctx_filter_set_caret(app, app->ctx_filter_len, true);
            changed = true;
        }
        break;
    case 'C':
        if (ctrl_down) {
            (void)app_ctx_filter_copy_to_clipboard(app);
            changed = true;
        }
        break;
    case 'V':
        if (ctrl_down) {
            changed = app_ctx_filter_paste_from_clipboard(app);
            n = app_ctx_rebuild_picks(app, picks);
            app_ctx_clamp_menu_selection(app, n);
        }
        break;
    case 'X':
        if (ctrl_down) {
            changed = app_ctx_filter_cut_to_clipboard(app);
            n = app_ctx_rebuild_picks(app, picks);
            app_ctx_clamp_menu_selection(app, n);
        }
        break;
    case VK_BACK:
        if (app_ctx_filter_has_selection(app)) {
            changed = app_ctx_filter_delete_selection(app);
        } else if (ctrl_down) {
            s32 word_start = app_ctx_filter_word_left(app, app->ctx_filter_caret);
            changed = app_ctx_filter_delete_range(app, word_start, app->ctx_filter_caret);
        } else if (app->ctx_filter_caret > 0) {
            changed = app_ctx_filter_delete_range(app, app->ctx_filter_caret - 1, app->ctx_filter_caret);
        }
        n = app_ctx_rebuild_picks(app, picks);
        app_ctx_clamp_menu_selection(app, n);
        break;
    case VK_DELETE:
        if (app_ctx_filter_has_selection(app)) {
            changed = app_ctx_filter_delete_selection(app);
        } else if (ctrl_down) {
            s32 word_end = app_ctx_filter_word_right(app, app->ctx_filter_caret);
            changed = app_ctx_filter_delete_range(app, app->ctx_filter_caret, word_end);
        } else if (app->ctx_filter_caret < app->ctx_filter_len) {
            changed = app_ctx_filter_delete_range(app, app->ctx_filter_caret, app->ctx_filter_caret + 1);
        }
        n = app_ctx_rebuild_picks(app, picks);
        app_ctx_clamp_menu_selection(app, n);
        break;
    case VK_LEFT: {
        s32 target = ctrl_down ? app_ctx_filter_word_left(app, app->ctx_filter_caret) : (app->ctx_filter_caret - 1);
        app_ctx_filter_set_caret(app, target, shift_down);
        changed = true;
    } break;
    case VK_RIGHT: {
        s32 target = ctrl_down ? app_ctx_filter_word_right(app, app->ctx_filter_caret) : (app->ctx_filter_caret + 1);
        app_ctx_filter_set_caret(app, target, shift_down);
        changed = true;
    } break;
    case VK_HOME:
        app_ctx_filter_set_caret(app, 0, shift_down);
        changed = true;
        break;
    case VK_END:
        app_ctx_filter_set_caret(app, app->ctx_filter_len, shift_down);
        changed = true;
        break;
    default:
        break;
    }

    if (changed) {
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return changed;
}

static void
app_compute_list_geometry(AppState *app, u32 client_w, u32 client_h, AppListGeometry *g)
{
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    const f32 border_thickness = 1.0f;
    const f32 outer_padding = 1.5f * s;
    const f32 header_gap = 2.0f * s;
    const f32 footer_gap = 8.5f * s;
    const f32 footer_h = 16.0f * s;

    UiRect window_rect = ui_rect(0.0f, 0.0f, (f32)client_w, (f32)client_h);
    UiRect content_rect = ui_inset(window_rect, border_thickness, border_thickness, border_thickness, border_thickness);
    UiRect top_bar_rect = ui_rect(content_rect.x + outer_padding, content_rect.y + outer_padding, content_rect.w - outer_padding * 2.0f, 48.0f * s);
    UiRect footer_rect = ui_rect(content_rect.x + outer_padding + 3.0f * s,
                                 content_rect.y + content_rect.h - footer_gap - footer_h,
                                 300.0f * s, footer_h);
    f32 list_top = top_bar_rect.y + top_bar_rect.h + header_gap;
    f32 list_h = footer_rect.y - footer_gap - list_top;
    if (list_h < 0.0f) {
        list_h = 0.0f;
    }
    UiRect list_rect = ui_rect(content_rect.x + outer_padding, list_top, content_rect.w - outer_padding * 2.0f, list_h);
    g->list_content_rect = ui_inset(list_rect, 5.0f * s, 4.0f * s, 5.0f * s, 4.0f * s);
    g->row_step = app_results_row_step(app);
    g->first_row_y = g->list_content_rect.y + 2.5f * s;
    g->rows_per_page = app_result_rows_per_page(app);
    bool show_scrollbar = ((s32)app->results.count > g->rows_per_page);
    f32 scrollbar_track_w = 8.0f * s;
    f32 scrollbar_inset = 6.0f * s;
    f32 scrollbar_reserved_w = show_scrollbar ? (scrollbar_track_w + scrollbar_inset * 2.0f) : 0.0f;
    g->content_right = g->list_content_rect.x + g->list_content_rect.w - scrollbar_reserved_w;
    g->start_index = app->results_top_index;
}

static void
app_center_window(AppState *app)
{
    RECT work = {0};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    s32 width = (s32)((f32)LAUNCHER_BASE_WINDOW_W * s + 0.5f);
    s32 height = (s32)((f32)LAUNCHER_BASE_WINDOW_H * s + 0.5f);
    app_window_size_for_rows(app, &width, &height);
    int work_w = work.right - work.left;
    int work_h = work.bottom - work.top;
    int x = work.left + (work_w - width) / 2;
    int y = work.top + (work_h - height) / 2;
    SetWindowPos(app->hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
}

static void
app_show(AppState *app)
{
    app->visible = true;
    debug_log_wide(L"app_show");
    app->query_anchor = 0;
    app_query_set_caret(app, app->query_length, true);
    app->query_scroll_x = 0.0f;
    app_center_window(app);
    ShowWindow(app->hwnd, SW_SHOW);
    SetForegroundWindow(app->hwnd);
    SetFocus(app->hwnd);
    InvalidateRect(app->hwnd, NULL, FALSE);
}

static void
app_hide(AppState *app)
{
    app_ctx_menu_close(app);
    app->visible = false;
    app->hover_result_index = -1;
    app->hover_lnk_chip_row = -1;
    debug_log_wide(L"app_hide");
    ShowWindow(app->hwnd, SW_HIDE);
}

static void
app_toggle(AppState *app)
{
    if (app->visible) {
        app_hide(app);
    } else {
        app_show(app);
    }
}

static void
app_set_install_dir(AppState *app)
{
    GetModuleFileNameW(NULL, app->install_dir, array_count(app->install_dir));
    PathRemoveFileSpecW(app->install_dir);
}

static void
app_init_catalog(AppState *app)
{
    if (app->catalog_arena.base) {
        arena_destroy(&app->catalog_arena);
    }
    app->catalog_arena = arena_create(gigabytes(1), megabytes(4));
    app_catalog_build(&app->catalog_arena, app->install_dir, &app->app_catalog, &app->catalog_aliases);
}

static void
app_refresh_results(AppState *app)
{
    app_ctx_menu_close(app);
    arena_reset(&app->results_arena);

    char lower_query[LAUNCHER_MAX_QUERY];
    strcpy_s(lower_query, sizeof(lower_query), app->query);
    lowercase_ascii_in_place(lower_query);

    if (app->mode == SearchMode_Apps) {
        app->results = fuzzy_rank_items(&app->results_arena, lower_query, app->app_catalog.items, app->app_catalog.count, LAUNCHER_MAX_RESULTS);
    } else {
        EverythingQueryResult query = everything_query_files(&app->results_arena, app->query, 128, &app->catalog_aliases);
        app->everything_available = query.available;
        /*
         * Everything honors * and ? as globs; fuzzy_rank_items uses subsequence matching and
         * treats those as literals, which filters out every path. Skip fuzzy when globs are used.
         */
        if (everything_query_has_glob_wildcards(app->query)) {
            app->results =
                fuzzy_pass_through_items(&app->results_arena, query.items.items, query.items.count, LAUNCHER_MAX_RESULTS);
        } else {
            app->results = fuzzy_rank_items(&app->results_arena, lower_query, query.items.items, query.items.count,
                                            LAUNCHER_MAX_RESULTS);
        }
    }

    if (app->results.count == 0) {
        app->selected_index = -1;
    } else {
        app->selected_index = 0;
    }
    app_clamp_result_view(app);
    if (app->results.count == 0 || app->hover_result_index >= (s32)app->results.count) {
        app->hover_result_index = -1;
    }
    if (app->results.count == 0 || app->hover_lnk_chip_row >= (s32)app->results.count) {
        app->hover_lnk_chip_row = -1;
    }
}

static f32
app_results_row_step(AppState *app)
{
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    f32 row_step = app->text_results.line_height * 2.0f + 12.0f * s;
    f32 min_step = 48.0f * s;
    if (row_step < min_step) {
        row_step = min_step;
    }
    return row_step;
}

static s32
app_effective_visible_rows(AppState *app)
{
    s32 rows = app->max_visible_rows;
    if (rows < LAUNCHER_MIN_VISIBLE_ROWS) {
        rows = LAUNCHER_MIN_VISIBLE_ROWS;
    }
    if (rows > LAUNCHER_MAX_VISIBLE_ROWS) {
        rows = LAUNCHER_MAX_VISIBLE_ROWS;
    }
    return rows;
}

static s32
app_result_rows_per_page(AppState *app)
{
    return app_effective_visible_rows(app);
}

static void
app_window_size_for_rows(AppState *app, s32 *out_width, s32 *out_height)
{
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    s32 rows = app_effective_visible_rows(app);
    f32 row_step = app_results_row_step(app);
    f32 list_h = row_step * (f32)rows + 24.0f * s;
    *out_width = (s32)((f32)LAUNCHER_BASE_WINDOW_W * s + 0.5f);
    *out_height = (s32)(list_h + 98.0f * s + 0.5f);
}

static void
app_clamp_results_top_bounds(AppState *app)
{
    if (app->results.count == 0) {
        app->results_top_index = 0;
        return;
    }
    s32 rows_per_page = app_result_rows_per_page(app);
    s32 max_top = (s32)app->results.count - rows_per_page;
    if (max_top < 0) {
        max_top = 0;
    }
    if (app->results_top_index < 0) {
        app->results_top_index = 0;
    }
    if (app->results_top_index > max_top) {
        app->results_top_index = max_top;
    }
}

static void
app_clamp_result_view(AppState *app)
{
    app_clamp_results_top_bounds(app);
    if (app->results.count == 0) {
        return;
    }

    s32 rows_per_page = app_result_rows_per_page(app);
    s32 max_top = (s32)app->results.count - rows_per_page;
    if (max_top < 0) {
        max_top = 0;
    }

    if (app->selected_index < app->results_top_index) {
        app->results_top_index = app->selected_index;
    }
    if (app->selected_index >= app->results_top_index + rows_per_page) {
        app->results_top_index = app->selected_index - rows_per_page + 1;
    }
    if (app->results_top_index < 0) {
        app->results_top_index = 0;
    }
    if (app->results_top_index > max_top) {
        app->results_top_index = max_top;
    }
}

static void
app_switch_mode(AppState *app)
{
    app->mode = (app->mode == SearchMode_Apps) ? SearchMode_Files : SearchMode_Apps;
    app_refresh_results(app);
}

static void
app_scroll_results_list(AppState *app, s32 wheel_delta)
{
    if (!app || wheel_delta == 0 || app->results.count == 0) {
        return;
    }
    s32 rows = app_result_rows_per_page(app);
    if ((s32)app->results.count <= rows) {
        return;
    }
    /* One result row per wheel message (direction only). */
    s32 lines = wheel_delta > 0 ? 1 : -1;
    app->results_top_index -= lines;
    app_clamp_results_top_bounds(app);
}

static s32
app_hit_test_result_index(AppState *app, f32 mx, f32 my, u32 client_w, u32 client_h)
{
    if (app->results.count == 0) {
        return -1;
    }
    AppListGeometry g;
    app_compute_list_geometry(app, client_w, client_h, &g);

    if (mx < g.list_content_rect.x || my < g.list_content_rect.y) {
        return -1;
    }
    if (mx >= g.content_right || my >= g.list_content_rect.y + g.list_content_rect.h) {
        return -1;
    }

    if (my < g.first_row_y) {
        return -1;
    }
    f32 rel_y = my - g.first_row_y;
    if (g.row_step <= 0.0f) {
        return -1;
    }
    s32 slot = (s32)floorf(rel_y / g.row_step);
    if (slot < 0) {
        return -1;
    }
    s32 end_index = g.start_index + g.rows_per_page;
    if (end_index > (s32)app->results.count) {
        end_index = (s32)app->results.count;
    }
    s32 vis_count = end_index - g.start_index;
    if (slot >= vis_count) {
        return -1;
    }
    s32 idx = g.start_index + slot;
    if (idx < 0 || idx >= (s32)app->results.count) {
        return -1;
    }
    return idx;
}

static s32
app_hit_test_lnk_badge_row(AppState *app, f32 mx, f32 my, u32 client_w, u32 client_h)
{
    s32 row = app_hit_test_result_index(app, mx, my, client_w, client_h);
    if (row < 0) {
        return -1;
    }
    const LaunchItem *it = app->results.items[(u32)row].item;
    if (it->source != LaunchSource_StartMenuShortcut || !it->shortcut_path || !it->shortcut_path[0]) {
        return -1;
    }
    AppListGeometry g;
    app_compute_list_geometry(app, client_w, client_h, &g);
    s32 end_index = g.start_index + g.rows_per_page;
    if (end_index > (s32)app->results.count) {
        end_index = (s32)app->results.count;
    }
    if (row < g.start_index || row >= end_index) {
        return -1;
    }
    f32 row_top = g.first_row_y + (f32)(row - g.start_index) * g.row_step;
    f32 row_h = app_results_row_step(app);
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    const f32 shortcut_lnk_right_inset = 8.0f * s;
    ArenaTemp tmp = arena_temp_begin(&app->frame_arena);
    UiRect chip = ui_shortcut_lnk_badge_bounds(&app->frame_arena, &app->text_results, g.content_right - shortcut_lnk_right_inset,
                                               row_top, row_h, s, "LNK");
    arena_temp_end(tmp);
    if (chip.w <= 0.0f || chip.h <= 0.0f) {
        return -1;
    }
    if (mx >= chip.x && mx < chip.x + chip.w && my >= chip.y && my < chip.y + chip.h) {
        return row;
    }
    return -1;
}

static void
app_move_selection(AppState *app, s32 delta)
{
    if (app->results.count == 0) {
        app->selected_index = -1;
        return;
    }
    app->selected_index += delta;
    if (app->selected_index < 0) {
        app->selected_index = 0;
    }
    if ((u32)app->selected_index >= app->results.count) {
        app->selected_index = (s32)app->results.count - 1;
    }
    app_clamp_result_view(app);
}

static void
app_page_down(AppState *app)
{
    if (app->results.count == 0) {
        app->selected_index = -1;
        return;
    }
    s32 n = app_result_rows_per_page(app);
    s32 last = (s32)app->results.count - 1;
    s32 s = app->selected_index;
    if (s < 0) {
        s = 0;
    }

    if (n <= 1) {
        if (s < last) {
            s++;
        }
    } else {
        s32 step = n - 1;
        s32 last_vis = app->results_top_index + n - 1;
        if (last_vis > last) {
            last_vis = last;
        }
        if (s == app->results_top_index) {
            s = last_vis;
        } else {
            s += step;
            if (s > last) {
                s = last;
            }
        }
    }
    app->selected_index = s;
    app_clamp_result_view(app);
}

static void
app_page_up(AppState *app)
{
    if (app->results.count == 0) {
        app->selected_index = -1;
        return;
    }
    s32 n = app_result_rows_per_page(app);
    s32 s = app->selected_index;
    if (s < 0) {
        s = 0;
    }

    if (n <= 1) {
        if (s > 0) {
            s--;
        }
    } else {
        s32 step = n - 1;
        s32 last = (s32)app->results.count - 1;
        s32 last_vis = app->results_top_index + n - 1;
        if (last_vis > last) {
            last_vis = last;
        }
        if (s == last_vis) {
            s = app->results_top_index;
        } else {
            s -= step;
            if (s < 0) {
                s = 0;
            }
        }
    }
    app->selected_index = s;
    app_clamp_result_view(app);
}

static void
app_activate_selection(AppState *app, bool elevated)
{
    if (app->selected_index < 0 || (u32)app->selected_index >= app->results.count) {
        return;
    }
    const LaunchItem *item = app->results.items[app->selected_index].item;
    if (platform_launch_item(item, elevated)) {
        app_hide(app);
    }
}

static void
draw_text_line_font(AppState *app, KbTextSystem *font, Arena *frame, f32 x, f32 baseline_y, const char *text, RenderColor color)
{
    u32 slot = 0;
    void *want_srv = (void *)app->renderer.atlas_srv;
    if (font == &app->text_results) {
        slot = 1u;
        want_srv = (void *)app->renderer.atlas_srv_b;
    } else if (font == &app->text_ctx_menu) {
        slot = 2u;
        want_srv = (void *)app->renderer.atlas_srv_c;
    } else if (font == &app->text_query) {
        slot = 3u;
        want_srv = (void *)app->renderer.atlas_srv_d;
    }
    if (font->raster.atlas_dirty) {
        dx11_renderer_upload_atlas(&app->renderer, &font->raster, slot);
        ((FontRaster *)&font->raster)->atlas_dirty = false;
    }
    if (app->renderer.vertex_count > 0 && (void *)app->renderer.pending_text_srv != want_srv) {
        dx11_renderer_flush(&app->renderer);
    }
    app->renderer.pending_text_srv = (ID3D11ShaderResourceView *)want_srv;

    f32 shape_x = x;
    f32 shape_y = baseline_y;
    if (app->renderer.text_snap_pixels) {
        shape_x = floorf(x + 0.5f);
        shape_y = floorf(baseline_y + 0.5f);
    }

    ShapedText shaped = kb_text_shape(frame, font, text, shape_x, shape_y);
    if (app->renderer.text_snap_pixels) {
        kb_text_snap_shaped_quads_to_pixels(&shaped);
    }
    dx11_renderer_draw_text(&app->renderer, &shaped, color);
}

static void
draw_text_line(AppState *app, Arena *frame, f32 x, f32 baseline_y, const char *text, RenderColor color)
{
    draw_text_line_font(app, &app->text, frame, x, baseline_y, text, color);
}

static s32
app_find_icon_entry(AppState *app, const wchar_t *path, s32 icon_index)
{
    if (!app || !path || !path[0]) {
        return -1;
    }
    for (s32 i = 0; i < app->icon_cache_count; ++i) {
        AppIconEntry *entry = &app->icon_cache[i];
        if (entry->icon_index == icon_index && entry->path && _wcsicmp(entry->path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static s32
app_get_or_create_icon_entry(AppState *app, const wchar_t *path, s32 icon_index)
{
    s32 found = app_find_icon_entry(app, path, icon_index);
    if (found >= 0) {
        return found;
    }

    if (app->icon_cache_count < LAUNCHER_ICON_CACHE_CAPACITY) {
        s32 idx = app->icon_cache_count++;
        AppIconEntry *entry = &app->icon_cache[idx];
        ZeroMemory(entry, sizeof(*entry));
        entry->path = arena_wcsdup(&app->permanent_arena, path);
        entry->icon_index = icon_index;
        entry->generation = 1;
        entry->state = AppIconState_Missing;
        return idx;
    }

    s32 replace = 0;
    u32 oldest = 0xffffffffu;
    for (s32 i = 0; i < app->icon_cache_count; ++i) {
        if (app->icon_cache[i].last_used_frame < oldest) {
            oldest = app->icon_cache[i].last_used_frame;
            replace = i;
        }
    }
    AppIconEntry *entry = &app->icon_cache[replace];
    dx11_renderer_destroy_texture(&entry->texture);
    entry->path = arena_wcsdup(&app->permanent_arena, path);
    entry->icon_index = icon_index;
    entry->generation += 1;
    if (entry->generation == 0) {
        entry->generation = 1;
    }
    entry->state = AppIconState_Missing;
    entry->last_used_frame = app->frame_counter;
    return replace;
}

static void
app_item_resolved_icon_source(const LaunchItem *item, const wchar_t **out_path, s32 *out_index)
{
    /* Start menu .lnk: resolve icon like Explorer (IShellItem on the shortcut), not the raw target type. */
    if (item->source == LaunchSource_StartMenuShortcut && item->shortcut_path && item->shortcut_path[0]) {
        *out_path = item->shortcut_path;
        *out_index = -1;
        return;
    }
    *out_path = item->icon_path ? item->icon_path : item->launch_path;
    *out_index = item->icon_index;
}

static void
app_request_item_icon(AppState *app, const LaunchItem *item)
{
    if (!app || !item) {
        return;
    }
    const wchar_t *path = NULL;
    s32 icon_index = -1;
    app_item_resolved_icon_source(item, &path, &icon_index);
    if (!path || !path[0]) {
        return;
    }
    s32 idx = app_get_or_create_icon_entry(app, path, icon_index);
    if (idx < 0) {
        return;
    }
    AppIconEntry *entry = &app->icon_cache[idx];
    entry->last_used_frame = app->frame_counter;
    if (entry->state == AppIconState_Missing) {
        IconWorkerRequest request;
        ZeroMemory(&request, sizeof(request));
        request.entry_index = idx;
        request.generation = entry->generation;
        request.icon_index = entry->icon_index;
        request.icon_size = (s32)((f32)LAUNCHER_ICON_SIZE_PX * app->dpi_scale + 0.5f);
        if (request.icon_size < 8) {
            request.icon_size = 8;
        }
        wcsncpy_s(request.path, array_count(request.path), entry->path, _TRUNCATE);
        if (item->icon_path && item->icon_path[0]) {
            wcsncpy_s(request.path_fallback, array_count(request.path_fallback), item->icon_path, _TRUNCATE);
            request.icon_index_fallback = item->icon_index;
        } else if (item->icon_fallback_path && item->icon_fallback_path[0]) {
            wcsncpy_s(request.path_fallback, array_count(request.path_fallback), item->icon_fallback_path, _TRUNCATE);
            request.icon_index_fallback = item->icon_fallback_index;
        } else {
            request.path_fallback[0] = 0;
        }
        if (icon_worker_submit(&app->icon_worker, &request)) {
            entry->state = AppIconState_Queued;
        } else {
            entry->state = AppIconState_Failed;
        }
    }
}

static void
app_process_icon_completions(AppState *app, s32 budget)
{
    if (!app || budget <= 0) {
        return;
    }
    IconWorkerResult result;
    while (budget-- > 0 && icon_worker_take_completed(&app->icon_worker, &result)) {
        if (result.entry_index < 0 || result.entry_index >= app->icon_cache_count) {
            icon_worker_free_result(&result);
            continue;
        }
        AppIconEntry *entry = &app->icon_cache[result.entry_index];
        if (entry->generation != result.generation) {
            icon_worker_free_result(&result);
            continue;
        }
        if (!result.success || !result.pixels) {
            entry->state = AppIconState_Failed;
            icon_worker_free_result(&result);
            continue;
        }
        dx11_renderer_destroy_texture(&entry->texture);
        if (dx11_renderer_create_texture_rgba(&app->renderer, result.width, result.height, result.pixels, &entry->texture)) {
            entry->state = AppIconState_Ready;
        } else {
            entry->state = AppIconState_Failed;
        }
        icon_worker_free_result(&result);
    }
}

static void
app_shutdown_icon_cache(AppState *app)
{
    if (!app) {
        return;
    }
    icon_worker_shutdown(&app->icon_worker);
    for (s32 i = 0; i < app->icon_cache_count; ++i) {
        dx11_renderer_destroy_texture(&app->icon_cache[i].texture);
    }
    app->icon_cache_count = 0;
}

static u32
app_primary_monitor_dpi(void)
{
    POINT pt;
    pt.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    pt.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) {
        return (u32)USER_DEFAULT_SCREEN_DPI;
    }
    UINT dpix = USER_DEFAULT_SCREEN_DPI;
    UINT dpiy = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpix, &dpiy)) || dpix == 0) {
        return (u32)USER_DEFAULT_SCREEN_DPI;
    }
    return (u32)dpix;
}

static bool
app_load_text_systems(AppState *app, const wchar_t *font_path)
{
    f32 ui_h = LAUNCHER_UI_FONT_PX * app->dpi_scale;
    f32 res_h = LAUNCHER_RESULTS_FONT_PX * app->dpi_scale;
    f32 query_h = LAUNCHER_QUERY_FONT_PX * app->dpi_scale;
    f32 ctx_h = LAUNCHER_CTX_MENU_FONT_PX * app->dpi_scale;
    if (!kb_text_init(&app->text, font_path, ui_h)) {
        return false;
    }
    dx11_renderer_upload_atlas(&app->renderer, &app->text.raster, 0);
    app->text.raster.atlas_dirty = false;
    if (!kb_text_init(&app->text_results, font_path, res_h)) {
        kb_text_shutdown(&app->text);
        return false;
    }
    dx11_renderer_upload_atlas(&app->renderer, &app->text_results.raster, 1);
    app->text_results.raster.atlas_dirty = false;
    if (!kb_text_init(&app->text_query, font_path, query_h)) {
        kb_text_shutdown(&app->text_results);
        kb_text_shutdown(&app->text);
        return false;
    }
    dx11_renderer_upload_atlas(&app->renderer, &app->text_query.raster, 3);
    app->text_query.raster.atlas_dirty = false;
    if (!kb_text_init(&app->text_ctx_menu, font_path, ctx_h)) {
        kb_text_shutdown(&app->text_query);
        kb_text_shutdown(&app->text_results);
        kb_text_shutdown(&app->text);
        return false;
    }
    dx11_renderer_upload_atlas(&app->renderer, &app->text_ctx_menu.raster, 2);
    app->text_ctx_menu.raster.atlas_dirty = false;
    return true;
}

static void
app_unload_text_systems(AppState *app)
{
    kb_text_shutdown(&app->text_ctx_menu);
    kb_text_shutdown(&app->text_query);
    kb_text_shutdown(&app->text_results);
    kb_text_shutdown(&app->text);
}

static void
app_invalidate_icon_textures(AppState *app)
{
    for (s32 i = 0; i < app->icon_cache_count; ++i) {
        dx11_renderer_destroy_texture(&app->icon_cache[i].texture);
        app->icon_cache[i].state = AppIconState_Missing;
        app->icon_cache[i].generation += 1;
        if (app->icon_cache[i].generation == 0) {
            app->icon_cache[i].generation = 1;
        }
    }
}

static void
draw_text_line_clamped_font(AppState *app, KbTextSystem *font, f32 x, f32 baseline_y, const char *text, RenderColor color, f32 max_width)
{
    if (!text || !text[0] || max_width <= 4.0f) {
        return;
    }

    ArenaTemp temp = arena_temp_begin(&app->frame_arena);
    ShapedText full = kb_text_shape(&app->frame_arena, font, text, 0.0f, 0.0f);
    arena_temp_end(temp);
    if (full.width <= max_width) {
        draw_text_line_font(app, font, &app->frame_arena, x, baseline_y, text, color);
        return;
    }

    const char *ellipsis = "...";
    temp = arena_temp_begin(&app->frame_arena);
    ShapedText ell = kb_text_shape(&app->frame_arena, font, ellipsis, 0.0f, 0.0f);
    arena_temp_end(temp);
    if (ell.width >= max_width) {
        return;
    }

    size_t length = strlen(text);
    if (length >= 1024) {
        length = 1023;
    }
    char buffer[1024];
    /* Longest prefix (whole UTF-8 codepoints) that fits max_width, then "...". */
    size_t starts[1024];
    size_t nchar = 0;
    utf8_prefix_byte_offsets(text, length, starts, &nchar, array_count(starts));

    size_t lo = 0;
    size_t hi = nchar;
    size_t bestk = 0;
    while (lo <= hi) {
        size_t k = lo + ((hi - lo) / 2);
        size_t bend = starts[k];
        memcpy(buffer, text, bend);
        buffer[bend] = 0;
        temp = arena_temp_begin(&app->frame_arena);
        ShapedText prefix = kb_text_shape(&app->frame_arena, font, buffer, 0.0f, 0.0f);
        arena_temp_end(temp);
        if (prefix.width + ell.width <= max_width) {
            bestk = k;
            lo = k + 1;
        } else {
            if (k == 0) {
                break;
            }
            hi = k - 1;
        }
    }

    size_t bend = starts[bestk];
    memcpy(buffer, text, bend);
    buffer[bend] = 0;
    strcat_s(buffer, sizeof(buffer), ellipsis);
    draw_text_line_font(app, font, &app->frame_arena, x, baseline_y, buffer, color);
}

static void
draw_text_line_clamped(AppState *app, f32 x, f32 baseline_y, const char *text, RenderColor color, f32 max_width)
{
    draw_text_line_clamped_font(app, &app->text, x, baseline_y, text, color, max_width);
}

static void
app_flush_ui_draw_list(AppState *app, UiDrawList *draw_list, u32 win_w, u32 win_h)
{
    UiRect clip_stack[64];
    int clip_sp = 0;
    clip_stack[0] = ui_rect(0.0f, 0.0f, (f32)win_w, (f32)win_h);

    for (u32 i = 0; i < draw_list->count; ++i) {
        UiDrawCmd *cmd = &draw_list->commands[i];
        if (cmd->type == UiDrawCmdType_ClipPush) {
            dx11_renderer_flush(&app->renderer);
            if (clip_sp < (int)array_count(clip_stack) - 1) {
                clip_sp += 1;
                clip_stack[clip_sp] = ui_rect_intersect(clip_stack[clip_sp - 1], cmd->clip.rect);
            }
            u32 L = 0;
            u32 T = 0;
            u32 R = 0;
            u32 B = 0;
            ui_rect_to_scissor_pixels(clip_stack[clip_sp], win_w, win_h, &L, &T, &R, &B);
            dx11_renderer_set_scissor_u32(&app->renderer, L, T, R, B);
            continue;
        }
        if (cmd->type == UiDrawCmdType_ClipPop) {
            dx11_renderer_flush(&app->renderer);
            if (clip_sp > 0) {
                clip_sp -= 1;
            }
            u32 L = 0;
            u32 T = 0;
            u32 R = 0;
            u32 B = 0;
            ui_rect_to_scissor_pixels(clip_stack[clip_sp], win_w, win_h, &L, &T, &R, &B);
            dx11_renderer_set_scissor_u32(&app->renderer, L, T, R, B);
            continue;
        }
        if (cmd->type == UiDrawCmdType_Rect) {
            dx11_renderer_draw_rect(&app->renderer,
                                    cmd->rect.rect.x,
                                    cmd->rect.rect.y,
                                    cmd->rect.rect.w,
                                    cmd->rect.rect.h,
                                    cmd->rect.color);
            continue;
        }
        if (cmd->type == UiDrawCmdType_Text) {
            KbTextSystem *font = cmd->text.font ? cmd->text.font : &app->text;
            if (cmd->text.max_width > 0.0f) {
                draw_text_line_clamped_font(app, font, cmd->text.x, cmd->text.baseline_y, cmd->text.text, cmd->text.color, cmd->text.max_width);
            } else {
                draw_text_line_font(app, font, &app->frame_arena, cmd->text.x, cmd->text.baseline_y, cmd->text.text, cmd->text.color);
            }
            continue;
        }
        if (cmd->type == UiDrawCmdType_Image) {
            ID3D11ShaderResourceView *want_srv = (ID3D11ShaderResourceView *)cmd->image.texture_srv;
            if (app->renderer.vertex_count > 0 && app->renderer.pending_text_srv != want_srv) {
                dx11_renderer_flush(&app->renderer);
            }
            app->renderer.pending_text_srv = want_srv;
            dx11_renderer_draw_image(&app->renderer,
                                     cmd->image.rect.x,
                                     cmd->image.rect.y,
                                     cmd->image.rect.w,
                                     cmd->image.rect.h,
                                     cmd->image.tint);
        }
    }
}

static void
app_render(AppState *app)
{
    if (!app->visible) {
        return;
    }

    RECT rect;
    GetClientRect(app->hwnd, &rect);
    u32 width = (u32)(rect.right - rect.left);
    u32 height = (u32)(rect.bottom - rect.top);

    arena_reset(&app->frame_arena);
    app->frame_counter += 1;
    app_process_icon_completions(app, 8);
    dx11_renderer_begin(&app->renderer, (RenderColor){0.038f, 0.044f, 0.058f, 1.0f});
    UiDrawList draw_list = {0};
    ui_drawlist_begin(&draw_list, &app->frame_arena, 8192);
    UiTheme theme = ui_theme_default();

    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    const f32 border_thickness = 1.0f;
    const f32 outer_padding = 1.5f * s;
    const f32 header_gap = 2.0f * s;
    const f32 footer_gap = 8.5f * s;
    const f32 footer_h = 16.0f * s;
    const RenderColor border_color = (RenderColor){0.20f, 0.50f, 0.95f, 1.0f};

    UiRect window_rect = ui_rect(0.0f, 0.0f, (f32)width, (f32)height);
    UiRect content_rect = ui_inset(window_rect, border_thickness, border_thickness, border_thickness, border_thickness);
    UiRect top_bar_rect = ui_rect(content_rect.x + outer_padding, content_rect.y + outer_padding, content_rect.w - outer_padding * 2.0f, 48.0f * s);
    UiRect footer_rect = ui_rect(content_rect.x + outer_padding + 3.0f * s,
                                 content_rect.y + content_rect.h - footer_gap - footer_h,
                                 300.0f * s, footer_h);
    f32 list_top = top_bar_rect.y + top_bar_rect.h + header_gap;
    f32 list_h = footer_rect.y - footer_gap - list_top;
    if (list_h < 0.0f) {
        list_h = 0.0f;
    }
    UiRect list_rect = ui_rect(content_rect.x + outer_padding, list_top, content_rect.w - outer_padding * 2.0f, list_h);
    UiRect list_content_rect = ui_inset(list_rect, 5.0f * s, 4.0f * s, 5.0f * s, 4.0f * s);
    const f32 top_row_h = 30.0f * s;
    const f32 top_row_pad_left = 9.0f * s;
    const f32 top_row_pad_right = 5.0f * s;
    UiRect top_row_rect = ui_rect(top_bar_rect.x + top_row_pad_left,
                                  top_bar_rect.y + (top_bar_rect.h - top_row_h) * 0.5f,
                                  top_bar_rect.w - top_row_pad_left - top_row_pad_right,
                                  top_row_h);
    UiTextInputControl input_control = {0};
    input_control.bounds = top_row_rect;
    input_control.text = app->query;
    input_control.placeholder = "Type to search...";
    input_control.has_text = (app->query_length > 0);
    input_control.scroll_x = &app->query_scroll_x;
    input_control.caret_index = app->query_caret;
    input_control.sel_start = app->query_anchor;
    input_control.sel_end = app->query_caret;
    input_control.caret_visible =
        app->caret_blink_on && !app->ctx_open && (GetFocus() == app->hwnd);

    ui_control_border(&draw_list, window_rect, border_thickness, border_color);
    ui_control_panel(&draw_list, content_rect, theme.bg_window);
    ui_control_panel(&draw_list, top_bar_rect, theme.bg_top_bar);
    ui_control_panel(&draw_list, list_rect, theme.bg_results_panel);
    ui_push_clip_rect(&draw_list, input_control.bounds);
    ui_control_text_input(&draw_list, &app->frame_arena, &input_control, &theme, &app->text_query, s);
    ui_pop_clip(&draw_list);

    f32 row_top = list_content_rect.y + 2.5f * s;
    f32 row_step = app_results_row_step(app);
    f32 row_height = row_step;
    s32 rows_per_page = app_result_rows_per_page(app);
    s32 start_index = app->results_top_index;
    bool show_scrollbar = ((s32)app->results.count > rows_per_page);
    f32 scrollbar_track_w = 8.0f * s;
    f32 scrollbar_inset = 6.0f * s;
    f32 scrollbar_reserved_w = show_scrollbar ? (scrollbar_track_w + scrollbar_inset * 2.0f) : 0.0f;
    f32 row_x = list_content_rect.x;
    f32 content_right = list_content_rect.x + list_content_rect.w - scrollbar_reserved_w;
    const f32 icon_size = (f32)LAUNCHER_ICON_SIZE_PX * s;
    const f32 icon_gap = 8.0f * s;
    f32 item_text_x = row_x + 6.0f * s + icon_size + icon_gap;
    f32 list_clip_w = content_right - list_content_rect.x;
    if (list_clip_w < 0.0f) {
        list_clip_w = 0.0f;
    }
    UiRect list_clip_rect = ui_rect(list_content_rect.x, list_content_rect.y, list_clip_w, list_content_rect.h);
    UiResultsListControl list_control = {0};
    list_control.bounds = list_rect;
    list_control.content_bounds = list_content_rect;
    list_control.selected_index = app->selected_index;
    list_control.start_index = start_index;
    list_control.rows_per_page = rows_per_page;
    list_control.total_count = (s32)app->results.count;
    list_control.show_scrollbar = show_scrollbar;
    list_control.row_height = row_height;
    list_control.row_step = row_step;
    ui_push_clip_rect(&draw_list, list_clip_rect);
    s32 end_index = start_index + rows_per_page;
    if (end_index > (s32)app->results.count) {
        end_index = (s32)app->results.count;
    }
    const char *shortcut_lnk_label = "LNK";
    f32 shortcut_lnk_chip_w = ui_shortcut_lnk_badge_chip_width(&app->frame_arena, &app->text_results, s, shortcut_lnk_label);
    const f32 shortcut_lnk_gap = 6.0f * s;
    const f32 shortcut_lnk_right_inset = 8.0f * s;
    for (s32 i = start_index; i < end_index; ++i) {
        bool selected = (i == app->selected_index);
        bool hover = (i == app->hover_result_index);
        if (selected) {
            ui_control_results_row(&draw_list, ui_rect(row_x, row_top, content_right - row_x, list_control.row_height), true, theme.row_selected_bg);
        } else if (hover) {
            ui_control_results_row(&draw_list, ui_rect(row_x, row_top, content_right - row_x, list_control.row_height), true, theme.row_hover_bg);
        }

        const LaunchItem *item = app->results.items[(u32)i].item;
        app_request_item_icon(app, item);
        const wchar_t *icon_lookup_path = NULL;
        s32 icon_lookup_index = -1;
        app_item_resolved_icon_source(item, &icon_lookup_path, &icon_lookup_index);
        s32 icon_entry_idx = app_find_icon_entry(app, icon_lookup_path, icon_lookup_index);
        f32 icon_x = row_x + 6.0f * s;
        f32 icon_y = row_top + (row_height - icon_size) * 0.5f;
        if (icon_entry_idx >= 0 && app->icon_cache[icon_entry_idx].state == AppIconState_Ready && app->icon_cache[icon_entry_idx].texture.srv) {
            ui_draw_image(&draw_list,
                          ui_rect(icon_x, icon_y, icon_size, icon_size),
                          (void *)app->icon_cache[icon_entry_idx].texture.srv,
                          (RenderColor){1.0f, 1.0f, 1.0f, 1.0f});
        } else {
            ui_draw_rect(&draw_list, ui_rect(icon_x, icon_y, icon_size, icon_size), (RenderColor){0.20f, 0.24f, 0.30f, 0.8f});
        }
        f32 title_baseline = row_top + app->text_results.pixel_height + 2.0f * s;
        f32 subtitle_baseline = title_baseline + app->text_results.line_height;
        f32 row_text_right = content_right - shortcut_lnk_right_inset;
        f32 title_max_w = row_text_right - item_text_x;
        if (item->source == LaunchSource_StartMenuShortcut) {
            title_max_w = row_text_right - shortcut_lnk_chip_w - shortcut_lnk_gap - item_text_x;
            if (title_max_w < 24.0f * s) {
                title_max_w = 24.0f * s;
            }
        }
        ui_draw_text_font_clamped(&draw_list, item_text_x, title_baseline, item->display_name, theme.fg_primary, &app->text_results,
                                  title_max_w);
        RenderColor path_fg = theme.fg_secondary;
        if (selected || hover) {
            path_fg = theme.mode_pill_fg;
        }
        if (item->subtitle) {
            ui_draw_text_font_clamped(&draw_list, item_text_x, subtitle_baseline, item->subtitle, path_fg, &app->text_results,
                                      title_max_w);
        }
        if (item->source == LaunchSource_StartMenuShortcut) {
            bool lnk_hot = (i == app->hover_lnk_chip_row);
            ui_control_shortcut_lnk_badge(&draw_list,
                                          &app->frame_arena,
                                          &app->text_results,
                                          content_right - shortcut_lnk_right_inset,
                                          row_top,
                                          row_height,
                                          s,
                                          shortcut_lnk_label,
                                          lnk_hot,
                                          (RenderColor){0.0f, 0.0f, 0.0f, 0.0f},
                                          path_fg,
                                          theme.mode_pill_bg,
                                          theme.mode_pill_fg);
        }
        row_top += row_step;
    }
    list_control.end_index = end_index;

    if (app->results.count == 0) {
        ui_draw_text_font(&draw_list,
                          list_content_rect.x + 2.0f * s,
                          list_content_rect.y + 2.5f * s + app->text_results.pixel_height,
                          "No matches",
                          theme.fg_secondary,
                          &app->text_results);
    }
    ui_pop_clip(&draw_list);

    UiScrollbarControl scrollbar = {0};
    scrollbar.bounds = list_control.bounds;
    scrollbar.visible = list_control.show_scrollbar;
    scrollbar.track_width = scrollbar_track_w;
    scrollbar.inset = scrollbar_inset;
    scrollbar.total_items = list_control.total_count;
    scrollbar.visible_items = list_control.rows_per_page;
    scrollbar.top_index = app->results_top_index;
    ui_control_scrollbar(&draw_list, &scrollbar, theme.scrollbar_track, theme.scrollbar_thumb);

    char diagnostics[160];
    u32 tm = app->renderer.text_render_mode;
    const char *tname = (tm < array_count(k_text_render_mode_names)) ? k_text_render_mode_names[tm] : "?";
    _snprintf_s(diagnostics, sizeof(diagnostics), _TRUNCATE, "Mode: %s | Results: %u | text:%s | g %.2f b %.2f",
        app->mode == SearchMode_Apps ? "Apps" : "Files",
        app->results.count,
        tname,
        (double)app->renderer.text_alpha_gamma,
        (double)app->renderer.text_gamma_blend);
    ui_control_footer(&draw_list, footer_rect, diagnostics, theme.fg_footer, &app->text_results);

    if (app->ctx_open) {
        CtxMenuPick ctx_picks[3];
        s32 ctx_n = app_ctx_rebuild_picks(app, ctx_picks);
        app_ctx_clamp_menu_selection(app, ctx_n);
        app_ctx_menu_update_layout(app, width, height, ctx_picks, ctx_n);
        f32 ctx_pad = 8.0f * s;
        ui_control_context_menu_panel(&draw_list, app->ctx_panel_rect, theme.bg_top_bar, border_color, 1.0f * s);
        /* Slightly darker than bg_top_bar so the filter reads as its own field */
        RenderColor ctx_filter_bg = {0.08f, 0.09f, 0.12f, 1.0f};
        ui_draw_rect(&draw_list, app->ctx_filter_bar_rect, ctx_filter_bg);
        UiTextInputControl ctx_input = {0};
        ctx_input.bounds = app->ctx_filter_text_rect;
        ctx_input.text = app->ctx_filter;
        ctx_input.placeholder = "Filter...";
        ctx_input.has_text = (app->ctx_filter_len > 0);
        ctx_input.scroll_x = &app->ctx_filter_scroll_x;
        ctx_input.caret_index = app->ctx_filter_caret;
        ctx_input.sel_start = app->ctx_filter_anchor;
        ctx_input.sel_end = app->ctx_filter_caret;
        ctx_input.caret_visible =
            app->caret_blink_on && app->ctx_open && (GetFocus() == app->hwnd);
        ui_push_clip_rect(&draw_list, app->ctx_filter_text_rect);
        ui_control_text_input(&draw_list, &app->frame_arena, &ctx_input, &theme, &app->text_ctx_menu, s);
        ui_pop_clip(&draw_list);
        if (app->ctx_target_index >= 0 && (u32)app->ctx_target_index < app->results.count) {
            const LaunchItem *ctx_item = app->results.items[(u32)app->ctx_target_index].item;
            for (s32 ci = 0; ci < ctx_n; ++ci) {
                f32 row_y = app->ctx_list_row0_y + (f32)ci * app->ctx_row_h;
                UiRect ctx_row = ui_rect(app->ctx_panel_rect.x + ctx_pad, row_y, app->ctx_panel_rect.w - ctx_pad * 2.0f, app->ctx_row_h);
                CtxAction act = (CtxAction)ctx_picks[ci].action;
                bool ctx_en = app_ctx_item_enabled(ctx_item, act);
                void *ctx_icon_srv = NULL;
                u32 act_i = (u32)ctx_picks[ci].action;
                if (act != CtxAction_Launch && act_i < CTX_MENU_ICON_COUNT && app->ctx_menu_icons[act_i].srv) {
                    ctx_icon_srv = (void *)app->ctx_menu_icons[act_i].srv;
                }
                ui_control_context_menu_item(&draw_list,
                                             &app->frame_arena,
                                             &theme,
                                             &app->text_ctx_menu,
                                             s,
                                             ctx_row,
                                             ci == app->ctx_selected,
                                             ci == app->ctx_hover_row,
                                             ctx_en,
                                             k_ctx_menu_labels[ctx_picks[ci].action],
                                             ctx_icon_srv);
            }
        }
    }

    app_flush_ui_draw_list(app, &draw_list, width, height);
    dx11_renderer_end(&app->renderer);
}

static void
app_cycle_text_gamma_preset(AppState *app)
{
    s_debug_text_gamma_preset_index = (s_debug_text_gamma_preset_index + 1) % k_debug_text_gamma_preset_count;
    f32 g = k_debug_text_gamma_presets[s_debug_text_gamma_preset_index];
    dx11_renderer_set_text_alpha_gamma(&app->renderer, g);
    debug_log_wide(L"Text alpha gamma: %.2f (%u/%u)",
                   (double)g,
                   (unsigned int)(s_debug_text_gamma_preset_index + 1u),
                   (unsigned int)k_debug_text_gamma_preset_count);
}

static void
app_cycle_text_gamma_blend_preset(AppState *app)
{
    s_debug_text_gamma_blend_index = (s_debug_text_gamma_blend_index + 1) % k_debug_text_gamma_blend_preset_count;
    f32 b = k_debug_text_gamma_blend_presets[s_debug_text_gamma_blend_index];
    dx11_renderer_set_text_gamma_blend(&app->renderer, b);
    debug_log_wide(L"Text gamma blend: %.2f (%u/%u) (1=full gamma, 0=raw alpha)",
                   (double)b,
                   (unsigned int)(s_debug_text_gamma_blend_index + 1u),
                   (unsigned int)k_debug_text_gamma_blend_preset_count);
}

static LRESULT CALLBACK
launcher_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AppState *app = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return 0;
    }
    case WM_HOTKEY:
        if (app && wParam == LAUNCHER_HOTKEY_ID) {
            debug_log_wide(L"WM_HOTKEY received");
            app_toggle(app);
        }
        return 0;
    case WM_LAUNCHER_CATALOG_FS_CHANGED:
        if (app && hwnd) {
            KillTimer(hwnd, IDT_CATALOG_FS_DEBOUNCE);
            SetTimer(hwnd, IDT_CATALOG_FS_DEBOUNCE, 450, NULL);
        }
        return 0;
    case WM_SETCURSOR:
        if (app && app->visible && !app->ctx_open && LOWORD(lParam) == HTCLIENT && app->hover_lnk_chip_row >= 0) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (app && app->renderer.device) {
            dx11_renderer_resize(&app->renderer, LOWORD(lParam), HIWORD(lParam));
            app_clamp_result_view(app);
        }
        return 0;
    case WM_DPICHANGED: {
        if (!app || !app->renderer.device) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        RECT const *r = (RECT const *)lParam;
        wchar_t font_path[MAX_PATH];
        _snwprintf_s(font_path, array_count(font_path), _TRUNCATE, L"%ls\\Fonts\\CascadiaMono.ttf", _wgetenv(L"WINDIR"));
        u32 old_dpi = app->dpi;
        f32 old_scale = app->dpi_scale;
        u32 new_dpi = (u32)LOWORD(wParam);
        if (new_dpi == 0) {
            new_dpi = (u32)USER_DEFAULT_SCREEN_DPI;
        }
        app_unload_text_systems(app);
        app->dpi = new_dpi;
        app->dpi_scale = (f32)new_dpi / (f32)USER_DEFAULT_SCREEN_DPI;
        if (!app_load_text_systems(app, font_path)) {
            debug_log_wide(L"app_load_text_systems failed after WM_DPICHANGED; restoring DPI");
            app->dpi = old_dpi;
            app->dpi_scale = old_scale;
            if (!app_load_text_systems(app, font_path)) {
                debug_log_wide(L"app_load_text_systems failed to restore fonts");
            }
        }
        app_invalidate_icon_textures(app);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_ACTIVATE:
        if (app && LOWORD(wParam) == WA_INACTIVE && app->visible) {
            app_hide(app);
        }
        return 0;
    case WM_CHAR:
        if (app && app->visible && app->ctx_open) {
            if (wParam >= 32u && wParam != 127u) {
                bool chg = false;
                if (wParam < 128u) {
                    chg = app_ctx_filter_insert_char(app, (char)wParam);
                } else {
                    wchar_t wch[2] = {(wchar_t)wParam, 0};
                    char utf8_tmp[8];
                    int nb = WideCharToMultiByte(CP_UTF8, 0, wch, -1, utf8_tmp, (int)sizeof(utf8_tmp), NULL, NULL);
                    if (nb > 1) {
                        chg = app_ctx_filter_insert_utf8(app, utf8_tmp);
                    }
                }
                if (chg) {
                    CtxMenuPick picks[3];
                    s32 n = app_ctx_rebuild_picks(app, picks);
                    app_ctx_clamp_menu_selection(app, n);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        if (app && app->visible && wParam >= 32 && wParam < 127) {
            if (app_query_insert_char(app, (char)wParam)) {
                app_refresh_results(app);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (app && app->visible) {
            bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (app->ctx_open) {
                app_ctx_menu_keydown(app, hwnd, wParam, ctrl_down, shift_down);
                return 0;
            }
            if (wParam == VK_APPS || (wParam == VK_F10 && shift_down && !ctrl_down)) {
                if (app->selected_index >= 0 && (u32)app->selected_index < app->results.count) {
                    RECT cr;
                    GetClientRect(hwnd, &cr);
                    u32 cw = (u32)(cr.right - cr.left);
                    u32 ch = (u32)(cr.bottom - cr.top);
                    AppListGeometry geo;
                    app_compute_list_geometry(app, cw, ch, &geo);
                    f32 sf = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
                    s32 rel = app->selected_index - geo.start_index;
                    f32 ay = geo.first_row_y;
                    if (rel >= 0 && rel < geo.rows_per_page) {
                        ay = geo.first_row_y + (f32)rel * geo.row_step;
                    }
                    f32 ax = geo.list_content_rect.x + 10.0f * sf;
                    app_ctx_menu_open_at(app, app->selected_index, ax, ay);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            bool query_changed = false;
            bool handled = true;
            switch (wParam) {
            case VK_ESCAPE:
                app_hide(app);
                break;
            case 'A':
                if (ctrl_down) {
                    app->query_anchor = 0;
                    app_query_set_caret(app, app->query_length, true);
                } else {
                    handled = false;
                }
                break;
            case 'C':
                if (ctrl_down) {
                    (void)app_query_copy_to_clipboard(app);
                } else {
                    handled = false;
                }
                break;
            case 'V':
                if (ctrl_down) {
                    query_changed = app_query_paste_from_clipboard(app);
                } else {
                    handled = false;
                }
                break;
            case 'X':
                if (ctrl_down) {
                    query_changed = app_query_cut_to_clipboard(app);
                } else {
                    handled = false;
                }
                break;
            case VK_BACK:
                if (app_query_has_selection(app)) {
                    query_changed = app_query_delete_selection(app);
                } else if (ctrl_down) {
                    s32 word_start = app_query_word_left(app, app->query_caret);
                    query_changed = app_query_delete_range(app, word_start, app->query_caret);
                } else if (app->query_caret > 0) {
                    query_changed = app_query_delete_range(app, app->query_caret - 1, app->query_caret);
                }
                break;
            case VK_DELETE:
                if (app_query_has_selection(app)) {
                    query_changed = app_query_delete_selection(app);
                } else if (ctrl_down) {
                    s32 word_end = app_query_word_right(app, app->query_caret);
                    query_changed = app_query_delete_range(app, app->query_caret, word_end);
                } else if (app->query_caret < app->query_length) {
                    query_changed = app_query_delete_range(app, app->query_caret, app->query_caret + 1);
                }
                break;
            case VK_TAB: app_switch_mode(app); break;
            case VK_DOWN: app_move_selection(app, 1); break;
            case VK_UP: app_move_selection(app, -1); break;
            case VK_NEXT: app_page_down(app); break;
            case VK_PRIOR: app_page_up(app); break;
            case VK_LEFT: {
                s32 target = ctrl_down ? app_query_word_left(app, app->query_caret) : (app->query_caret - 1);
                app_query_set_caret(app, target, shift_down);
            } break;
            case VK_RIGHT: {
                s32 target = ctrl_down ? app_query_word_right(app, app->query_caret) : (app->query_caret + 1);
                app_query_set_caret(app, target, shift_down);
            } break;
            case VK_HOME:
                app_query_set_caret(app, 0, shift_down);
                break;
            case VK_END:
                app_query_set_caret(app, app->query_length, shift_down);
                break;
            case VK_RETURN: app_activate_selection(app, ctrl_down && shift_down); break;
            case VK_F5: app_init_catalog(app); app_refresh_results(app); break;
            case VK_F6: {
                bool snap_on = dx11_renderer_toggle_text_pixel_snap(&app->renderer);
                debug_log_wide(L"Text pixel snap: %ls", snap_on ? L"on" : L"off");
            } break;
            case VK_F7:
                app_cycle_text_gamma_preset(app);
                break;
            case VK_F8:
                app_cycle_text_gamma_blend_preset(app);
                break;
            case VK_F9: {
                u32 m = dx11_renderer_cycle_text_render_mode(&app->renderer);
                const wchar_t *wname = L"?";
                if (m < array_count(k_text_render_mode_names)) {
                    switch (m) {
                    case 0: wname = L"legacy"; break;
                    case 1: wname = L"raw"; break;
                    case 2: wname = L"gammaA"; break;
                    case 3: wname = L"linBg (vs frame clear)"; break;
                    default: break;
                    }
                }
                debug_log_wide(L"Text render mode: %ls (%u/%u)", wname, (unsigned int)(m + 1), (unsigned int)TextRenderMode_Count);
            } break;
            default: handled = false; break;
            }
            if (query_changed) {
                app_refresh_results(app);
            }
            if (handled) {
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app && app->visible) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            RECT cr;
            GetClientRect(hwnd, &cr);
            s32 mx = (s32)(short)LOWORD(lParam);
            s32 my = (s32)(short)HIWORD(lParam);
            if (app->ctx_open) {
                s32 ch = app_ctx_hit_test(app, (f32)mx, (f32)my);
                s32 nh = (ch >= 0) ? ch : -1;
                bool inv = false;
                if (nh != app->ctx_hover_row) {
                    app->ctx_hover_row = nh;
                    inv = true;
                }
                if (app->hover_result_index >= 0) {
                    app->hover_result_index = -1;
                    inv = true;
                }
                if (app->hover_lnk_chip_row >= 0) {
                    app->hover_lnk_chip_row = -1;
                    inv = true;
                }
                if (inv) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else {
                u32 cw = (u32)(cr.right - cr.left);
                u32 ch = (u32)(cr.bottom - cr.top);
                s32 hit = app_hit_test_result_index(app, (f32)mx, (f32)my, cw, ch);
                s32 lnk_hit = app_hit_test_lnk_badge_row(app, (f32)mx, (f32)my, cw, ch);
                bool inv = false;
                if (hit != app->hover_result_index) {
                    app->hover_result_index = hit;
                    inv = true;
                }
                if (lnk_hit != app->hover_lnk_chip_row) {
                    app->hover_lnk_chip_row = lnk_hit;
                    inv = true;
                }
                if (inv) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (app && app->visible && (app->hover_result_index >= 0 || app->hover_lnk_chip_row >= 0)) {
            app->hover_result_index = -1;
            app->hover_lnk_chip_row = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (app && app->visible) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            u32 cw = (u32)(cr.right - cr.left);
            u32 ch = (u32)(cr.bottom - cr.top);
            s32 mx = (s32)(short)LOWORD(lParam);
            s32 my = (s32)(short)HIWORD(lParam);
            if (app->ctx_open) {
                s32 chit = app_ctx_hit_test(app, (f32)mx, (f32)my);
                if (chit == -2) {
                    app_ctx_menu_close(app);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            s32 hit = app_hit_test_result_index(app, (f32)mx, (f32)my, cw, ch);
            if (hit >= 0) {
                app->selected_index = hit;
                app_ctx_menu_open_at(app, hit, (f32)mx, (f32)my);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (app && app->visible) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            u32 cw = (u32)(cr.right - cr.left);
            u32 ch = (u32)(cr.bottom - cr.top);
            s32 mx = (s32)(short)LOWORD(lParam);
            s32 my = (s32)(short)HIWORD(lParam);
            if (app->ctx_open) {
                s32 chit = app_ctx_hit_test(app, (f32)mx, (f32)my);
                if (chit == -2) {
                    app_ctx_menu_close(app);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (chit == -1) {
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (chit >= 0) {
                    app->ctx_selected = chit;
                    app_ctx_activate_selection(app, false);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
            s32 lnk_row = app_hit_test_lnk_badge_row(app, (f32)mx, (f32)my, cw, ch);
            if (lnk_row >= 0) {
                const LaunchItem *lnk_item = app->results.items[(u32)lnk_row].item;
                if (lnk_item->shortcut_path && lnk_item->shortcut_path[0]) {
                    platform_show_file_properties(hwnd, lnk_item->shortcut_path);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            s32 hit = app_hit_test_result_index(app, (f32)mx, (f32)my, cw, ch);
            if (hit >= 0) {
                app->selected_index = hit;
                InvalidateRect(hwnd, NULL, FALSE);
                app_activate_selection(app, false);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (app && app->visible) {
            if (app->ctx_open) {
                return 0;
            }
            s16 wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (wheel_delta != 0) {
                app_scroll_results_list(app, (s32)wheel_delta);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        if (app && app->visible) {
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == IDT_CATALOG_FS_DEBOUNCE) {
            KillTimer(hwnd, IDT_CATALOG_FS_DEBOUNCE);
            if (app) {
                app_init_catalog(app);
                app_invalidate_icon_textures(app);
                app_refresh_results(app);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (app && app->visible) {
            u32 now = GetTickCount();
            if (now - app->caret_blink_tick_ms >= 530) {
                app->caret_blink_on = !app->caret_blink_on;
                app->caret_blink_tick_ms = now;
            }
            app_render(app);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (app && app->visible) {
            app_render(app);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool
app_init(AppState *app, HINSTANCE instance)
{
    ZeroMemory(app, sizeof(*app));
    debug_log_wide(L"app_init begin");
    app->mode = SearchMode_Apps;
    app->selected_index = -1;
    app->hover_result_index = -1;
    app->hover_lnk_chip_row = -1;
    app->results_top_index = 0;
    app->max_visible_rows = LAUNCHER_DEFAULT_VISIBLE_ROWS;
    app->everything_available = true;
    app->query[0] = 0;
    app->query_length = 0;
    app->query_caret = 0;
    app->query_anchor = 0;
    app->query_scroll_x = 0.0f;
    app->caret_blink_tick_ms = GetTickCount();
    app->caret_blink_on = true;
    app->dpi = app_primary_monitor_dpi();
    app->dpi_scale = (f32)app->dpi / (f32)USER_DEFAULT_SCREEN_DPI;

    app->permanent_arena = arena_create(gigabytes(1), megabytes(8));
    app->results_arena = arena_create(gigabytes(1), megabytes(4));
    app->frame_arena = arena_create(gigabytes(1), megabytes(4));

    app_set_install_dir(app);
    debug_log_wide(L"install_dir=%ls", app->install_dir);
    app_init_catalog(app);
    debug_log_wide(L"catalog built count=%u", app->app_catalog.count);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = launcher_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = LAUNCHER_WINDOW_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        debug_log_wide(L"RegisterClassExW failed %lu", GetLastError());
        return false;
    }
    debug_log_wide(L"window class registered");

    s32 initial_width = MulDiv(LAUNCHER_BASE_WINDOW_W, (int)app->dpi, USER_DEFAULT_SCREEN_DPI);
    s32 initial_height = MulDiv(LAUNCHER_BASE_WINDOW_H, (int)app->dpi, USER_DEFAULT_SCREEN_DPI);
    app->hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        LAUNCHER_WINDOW_CLASS,
        LAUNCHER_WINDOW_TITLE,
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        initial_width,
        initial_height,
        NULL,
        NULL,
        instance,
        app);
    if (!app->hwnd) {
        debug_log_wide(L"CreateWindowExW failed %lu", GetLastError());
        return false;
    }
    debug_log_wide(L"window created hwnd=%p", app->hwnd);

    if (!dx11_renderer_init(&app->renderer, app->hwnd, (u32)initial_width, (u32)initial_height)) {
        debug_log_wide(L"dx11_renderer_init failed");
        return false;
    }
    dx11_renderer_set_text_alpha_gamma(&app->renderer, k_debug_text_gamma_presets[s_debug_text_gamma_preset_index]);
    dx11_renderer_set_text_gamma_blend(&app->renderer, k_debug_text_gamma_blend_presets[s_debug_text_gamma_blend_index]);
    dx11_renderer_set_text_render_mode(&app->renderer, TextRenderMode_FullGammaAlpha);
    debug_log_wide(L"renderer initialized");

    if (!ctx_menu_icons_load(&app->renderer, app->install_dir, app->ctx_menu_icons)) {
        debug_log_wide(L"ctx_menu_icons_load failed (expected %ls\\data\\icons\\ctx_*.svg)", app->install_dir);
    }

    if (!icon_worker_init(&app->icon_worker)) {
        debug_log_wide(L"icon_worker_init failed");
        return false;
    }

    wchar_t font_path[MAX_PATH];
    _snwprintf_s(font_path, array_count(font_path), _TRUNCATE, L"%ls\\Fonts\\CascadiaMono.ttf", _wgetenv(L"WINDIR"));
    if (!app_load_text_systems(app, font_path)) {
        debug_log_wide(L"app_load_text_systems failed font=%ls", font_path);
        return false;
    }
    debug_log_wide(L"text initialized dpi=%u scale=%f", app->dpi, (double)app->dpi_scale);

    {
        s32 fit_w = 0;
        s32 fit_h = 0;
        app_window_size_for_rows(app, &fit_w, &fit_h);
        SetWindowPos(app->hwnd, NULL, 0, 0, fit_w, fit_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        dx11_renderer_resize(&app->renderer, (u32)fit_w, (u32)fit_h);
    }

    RegisterHotKey(app->hwnd, LAUNCHER_HOTKEY_ID, MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    debug_log_wide(L"hotkey registered");
    SetTimer(app->hwnd, 1, 16, NULL);
    app_refresh_results(app);
    catalog_watch_start(app->hwnd, app->install_dir);
    debug_log_wide(L"app_init complete results=%u", app->results.count);
    return true;
}

static void
app_shutdown(AppState *app)
{
    catalog_watch_stop();
    UnregisterHotKey(app->hwnd, LAUNCHER_HOTKEY_ID);
    app_shutdown_icon_cache(app);
    ctx_menu_icons_destroy(app->ctx_menu_icons);
    app_unload_text_systems(app);
    dx11_renderer_shutdown(&app->renderer);
    arena_destroy(&app->frame_arena);
    arena_destroy(&app->results_arena);
    arena_destroy(&app->catalog_arena);
    arena_destroy(&app->permanent_arena);
}

int WINAPI
wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR command_line, int show_code)
{
    (void)prev_instance;
    (void)command_line;
    (void)show_code;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        fatal_message(L"Failed to initialize COM.");
    }

    AppState *app = (AppState *)heap_alloc_zero(sizeof(AppState));
    if (!app) {
        fatal_message(L"Failed to allocate app state.");
    }
    g_app = app;
    if (!app_init(app, instance)) {
        fatal_message(L"Failed to initialize launcher.");
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app_shutdown(app);
    heap_free(app);
    g_app = NULL;
    CoUninitialize();
    return 0;
}
