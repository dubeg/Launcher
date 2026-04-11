#include "app.h"

#include "../core/base.h"
#include "../platform/catalog.h"
#include "../platform/everything_client.h"
#include "../platform/icon_worker.h"
#include "../platform/launch.h"
#include "../render/dx11_renderer.h"
#include "../search/fuzzy.h"
#include "../text/kb_text.h"
#include "../ui/ui.h"

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
#define LAUNCHER_UI_FONT_PX 24.0f
#define LAUNCHER_BASE_WINDOW_W 920
#define LAUNCHER_BASE_WINDOW_H 560
#define LAUNCHER_DEFAULT_VISIBLE_ROWS 10
#define LAUNCHER_MIN_VISIBLE_ROWS 1
#define LAUNCHER_MAX_VISIBLE_ROWS 50
#define LAUNCHER_ICON_SIZE_PX 20
#define LAUNCHER_ICON_CACHE_CAPACITY 512
#define LAUNCHER_ICON_QUEUE_CAPACITY 1024

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

typedef struct AppState {
    HWND hwnd;
    Arena permanent_arena;
    Arena catalog_arena;
    Arena results_arena;
    Arena frame_arena;
    Dx11Renderer renderer;
    KbTextSystem text;
    KbTextSystem text_results;
    LaunchItemArray app_catalog;
    SearchResultArray results;
    SearchMode mode;
    bool visible;
    bool everything_available;
    s32 selected_index;
    s32 hover_result_index;
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
} AppState;

static AppState *g_app = NULL;

static void app_refresh_results(AppState *app);
static s32 app_result_rows_per_page(AppState *app);
static f32 app_results_row_step(AppState *app);
static void app_scroll_results_list(AppState *app, s32 wheel_delta);
static s32 app_hit_test_result_index(AppState *app, f32 mx, f32 my, u32 client_w, u32 client_h);
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
    app->visible = false;
    app->hover_result_index = -1;
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
    app->catalog_arena = arena_create(gigabytes(1), megabytes(4));
    app_catalog_build(&app->catalog_arena, app->install_dir, &app->app_catalog);
}

static void
app_refresh_results(AppState *app)
{
    arena_reset(&app->results_arena);

    char lower_query[LAUNCHER_MAX_QUERY];
    strcpy_s(lower_query, sizeof(lower_query), app->query);
    lowercase_ascii_in_place(lower_query);

    if (app->mode == SearchMode_Apps) {
        app->results = fuzzy_rank_items(&app->results_arena, lower_query, app->app_catalog.items, app->app_catalog.count, LAUNCHER_MAX_RESULTS);
    } else {
        EverythingQueryResult query = everything_query_files(&app->results_arena, app->query, 128);
        app->everything_available = query.available;
        app->results = fuzzy_rank_items(&app->results_arena, lower_query, query.items.items, query.items.count, LAUNCHER_MAX_RESULTS);
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
    s32 lines = wheel_delta * 3 / WHEEL_DELTA;
    if (lines == 0) {
        lines = wheel_delta > 0 ? 1 : -1;
    }
    app->results_top_index -= lines;
    app_clamp_results_top_bounds(app);
}

static s32
app_hit_test_result_index(AppState *app, f32 mx, f32 my, u32 client_w, u32 client_h)
{
    if (app->results.count == 0) {
        return -1;
    }
    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    const f32 border_thickness = 1.0f;
    const f32 outer_padding = 3.0f * s;
    const f32 header_gap = 3.0f * s;
    const f32 footer_gap = 10.0f * s;
    const f32 footer_h = 16.0f * s;

    UiRect window_rect = ui_rect(0.0f, 0.0f, (f32)client_w, (f32)client_h);
    UiRect content_rect = ui_inset(window_rect, border_thickness, border_thickness, border_thickness, border_thickness);
    UiRect top_bar_rect = ui_rect(content_rect.x + outer_padding, content_rect.y + outer_padding, content_rect.w - outer_padding * 2.0f, 48.0f * s);
    UiRect footer_rect = ui_rect(content_rect.x + outer_padding + 4.0f * s,
                                 content_rect.y + content_rect.h - footer_gap - footer_h,
                                 300.0f * s, footer_h);
    f32 list_top = top_bar_rect.y + top_bar_rect.h + header_gap;
    f32 list_h = footer_rect.y - footer_gap - list_top;
    if (list_h < 0.0f) {
        list_h = 0.0f;
    }
    UiRect list_rect = ui_rect(content_rect.x + outer_padding, list_top, content_rect.w - outer_padding * 2.0f, list_h);
    UiRect list_content_rect = ui_inset(list_rect, 6.0f * s, 6.0f * s, 6.0f * s, 6.0f * s);

    s32 rows_per_page = app_result_rows_per_page(app);
    f32 row_step = app_results_row_step(app);
    bool show_scrollbar = ((s32)app->results.count > rows_per_page);
    f32 scrollbar_track_w = 8.0f * s;
    f32 scrollbar_inset = 6.0f * s;
    f32 scrollbar_reserved_w = show_scrollbar ? (scrollbar_track_w + scrollbar_inset * 2.0f) : 0.0f;
    f32 row_x = list_content_rect.x;
    f32 content_right = list_content_rect.x + list_content_rect.w - scrollbar_reserved_w;
    (void)row_x;

    if (mx < list_content_rect.x || my < list_content_rect.y) {
        return -1;
    }
    if (mx >= content_right || my >= list_content_rect.y + list_content_rect.h) {
        return -1;
    }

    f32 first_row_y = list_content_rect.y + 3.0f * s;
    if (my < first_row_y) {
        return -1;
    }
    f32 rel_y = my - first_row_y;
    if (row_step <= 0.0f) {
        return -1;
    }
    s32 slot = (s32)floorf(rel_y / row_step);
    if (slot < 0) {
        return -1;
    }
    s32 start_index = app->results_top_index;
    s32 end_index = start_index + rows_per_page;
    if (end_index > (s32)app->results.count) {
        end_index = (s32)app->results.count;
    }
    s32 vis_count = end_index - start_index;
    if (slot >= vis_count) {
        return -1;
    }
    s32 idx = start_index + slot;
    if (idx < 0 || idx >= (s32)app->results.count) {
        return -1;
    }
    return idx;
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
app_activate_selection(AppState *app)
{
    if (app->selected_index < 0 || (u32)app->selected_index >= app->results.count) {
        return;
    }
    const LaunchItem *item = app->results.items[app->selected_index].item;
    if (platform_launch_item(item)) {
        app_hide(app);
    }
}

static void
draw_text_line_font(AppState *app, KbTextSystem *font, Arena *frame, f32 x, f32 baseline_y, const char *text, RenderColor color)
{
    if (font->raster.atlas_dirty) {
        u32 slot = (font == &app->text_results) ? 1u : 0u;
        dx11_renderer_upload_atlas(&app->renderer, &font->raster, slot);
        ((FontRaster *)&font->raster)->atlas_dirty = false;
    }
    void *want_srv = (font == &app->text_results) ? (void *)app->renderer.atlas_srv_b : (void *)app->renderer.atlas_srv;
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
app_request_item_icon(AppState *app, const LaunchItem *item)
{
    if (!app || !item) {
        return;
    }
    const wchar_t *path = item->icon_path ? item->icon_path : item->launch_path;
    if (!path || !path[0]) {
        return;
    }
    s32 icon_index = item->icon_index;
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
    return true;
}

static void
app_unload_text_systems(AppState *app)
{
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
    memcpy(buffer, text, length);
    buffer[length] = 0;

    size_t low = 0;
    size_t high = length;
    size_t best = 0;
    while (low <= high) {
        size_t mid = low + ((high - low) / 2);
        buffer[mid] = 0;
        temp = arena_temp_begin(&app->frame_arena);
        ShapedText prefix = kb_text_shape(&app->frame_arena, font, buffer, 0.0f, 0.0f);
        arena_temp_end(temp);
        if (prefix.width + ell.width <= max_width) {
            best = mid;
            low = mid + 1;
        } else {
            if (mid == 0) {
                break;
            }
            high = mid - 1;
        }
    }

    buffer[best] = 0;
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
    dx11_renderer_begin(&app->renderer, (RenderColor){0.06f, 0.07f, 0.09f, 1.0f});
    UiDrawList draw_list = {0};
    ui_drawlist_begin(&draw_list, &app->frame_arena, 8192);
    UiTheme theme = ui_theme_default();

    f32 s = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    const f32 border_thickness = 1.0f;
    const f32 outer_padding = 3.0f * s;
    const f32 header_gap = 3.0f * s;
    const f32 footer_gap = 10.0f * s;
    const f32 footer_h = 16.0f * s;
    const RenderColor border_color = (RenderColor){0.20f, 0.50f, 0.95f, 1.0f};

    UiRect window_rect = ui_rect(0.0f, 0.0f, (f32)width, (f32)height);
    UiRect content_rect = ui_inset(window_rect, border_thickness, border_thickness, border_thickness, border_thickness);
    UiRect top_bar_rect = ui_rect(content_rect.x + outer_padding, content_rect.y + outer_padding, content_rect.w - outer_padding * 2.0f, 48.0f * s);
    UiRect footer_rect = ui_rect(content_rect.x + outer_padding + 4.0f * s,
                                 content_rect.y + content_rect.h - footer_gap - footer_h,
                                 300.0f * s, footer_h);
    f32 list_top = top_bar_rect.y + top_bar_rect.h + header_gap;
    f32 list_h = footer_rect.y - footer_gap - list_top;
    if (list_h < 0.0f) {
        list_h = 0.0f;
    }
    UiRect list_rect = ui_rect(content_rect.x + outer_padding, list_top, content_rect.w - outer_padding * 2.0f, list_h);
    UiRect list_content_rect = ui_inset(list_rect, 6.0f * s, 6.0f * s, 6.0f * s, 6.0f * s);
    const f32 top_row_h = 30.0f * s;
    const f32 top_row_pad_left = 11.0f * s;
    const f32 top_row_pad_right = 6.0f * s;
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
    input_control.caret_visible = app->caret_blink_on;

    ui_control_border(&draw_list, window_rect, border_thickness, border_color);
    ui_control_panel(&draw_list, content_rect, theme.bg_window);
    ui_control_panel(&draw_list, top_bar_rect, theme.bg_top_bar);
    ui_control_panel(&draw_list, list_rect, theme.bg_results_panel);
    ui_push_clip_rect(&draw_list, input_control.bounds);
    ui_control_text_input(&draw_list, &app->frame_arena, &input_control, &theme, &app->text_results, s);
    ui_pop_clip(&draw_list);

    f32 row_top = list_content_rect.y + 3.0f * s;
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
        const wchar_t *icon_path = item->icon_path ? item->icon_path : item->launch_path;
        s32 icon_entry_idx = app_find_icon_entry(app, icon_path, item->icon_index);
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
        ui_draw_text_font(&draw_list, item_text_x, title_baseline, item->display_name, theme.fg_primary, &app->text_results);
        if (item->subtitle) {
            ui_draw_text_font(&draw_list, item_text_x, subtitle_baseline, item->subtitle, theme.fg_secondary, &app->text_results);
        }
        row_top += row_step;
    }
    list_control.end_index = end_index;

    if (app->results.count == 0) {
        ui_draw_text_font(&draw_list,
                          list_content_rect.x + 2.0f * s,
                          list_content_rect.y + 3.0f * s + app->text_results.pixel_height,
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
            bool query_changed = false;
            bool handled = true;
            switch (wParam) {
            case VK_ESCAPE: app_hide(app); break;
            case 'A':
                if (ctrl_down) {
                    app->query_anchor = 0;
                    app_query_set_caret(app, app->query_length, true);
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
            case VK_RETURN: app_activate_selection(app); break;
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
            s32 hit = app_hit_test_result_index(app, (f32)mx, (f32)my, (u32)(cr.right - cr.left), (u32)(cr.bottom - cr.top));
            if (hit != app->hover_result_index) {
                app->hover_result_index = hit;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (app && app->visible && app->hover_result_index >= 0) {
            app->hover_result_index = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (app && app->visible) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            s32 mx = (s32)(short)LOWORD(lParam);
            s32 my = (s32)(short)HIWORD(lParam);
            s32 hit = app_hit_test_result_index(app, (f32)mx, (f32)my, (u32)(cr.right - cr.left), (u32)(cr.bottom - cr.top));
            if (hit >= 0) {
                app->selected_index = hit;
                InvalidateRect(hwnd, NULL, FALSE);
                app_activate_selection(app);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (app && app->visible) {
            s16 wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (wheel_delta != 0) {
                app_scroll_results_list(app, (s32)wheel_delta);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_TIMER:
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
    debug_log_wide(L"app_init complete results=%u", app->results.count);
    return true;
}

static void
app_shutdown(AppState *app)
{
    UnregisterHotKey(app->hwnd, LAUNCHER_HOTKEY_ID);
    app_shutdown_icon_cache(app);
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
