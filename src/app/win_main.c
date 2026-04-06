#include "app.h"

#include "../core/base.h"
#include "../platform/catalog.h"
#include "../platform/everything_client.h"
#include "../platform/launch.h"
#include "../render/dx11_renderer.h"
#include "../search/fuzzy.h"
#include "../text/kb_text.h"
#include "../ui/ui.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

#define LAUNCHER_RESULTS_FONT_PX 18.0f
#define LAUNCHER_DEFAULT_VISIBLE_ROWS 10
#define LAUNCHER_MIN_VISIBLE_ROWS 1
#define LAUNCHER_MAX_VISIBLE_ROWS 50

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
} AppState;

static AppState *g_app = NULL;

static void app_refresh_results(AppState *app);
static s32 app_result_rows_per_page(AppState *app);
static f32 app_results_row_step(AppState *app);
static f32 app_measure_text_width(AppState *app, KbTextSystem *font, const char *text);
static void app_clamp_result_view(AppState *app);
static void app_clamp_results_top_bounds(AppState *app);
static void app_window_size_for_rows(AppState *app, s32 *out_width, s32 *out_height);

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
    s32 width = 920;
    s32 height = 560;
    app_window_size_for_rows(app, &width, &height);
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.top + 80;
    SetWindowPos(app->hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
}

static void
app_show(AppState *app)
{
    app->visible = true;
    debug_log_wide(L"app_show");
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
        if (app->selected_index < 0) {
            app->selected_index = 0;
        }
        if ((u32)app->selected_index >= app->results.count) {
            app->selected_index = (s32)app->results.count - 1;
        }
    }
    app_clamp_result_view(app);
}

static f32
app_results_row_step(AppState *app)
{
    f32 row_step = app->text_results.line_height * 2.0f + 12.0f;
    if (row_step < 48.0f) {
        row_step = 48.0f;
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
    s32 rows = app_effective_visible_rows(app);
    f32 row_step = app_results_row_step(app);
    f32 list_h = row_step * (f32)rows + 36.0f;
    *out_width = 920;
    *out_height = (s32)(list_h + 122.0f + 0.5f);
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

    ShapedText shaped = kb_text_shape(frame, font, text, x, baseline_y);
    dx11_renderer_draw_text(&app->renderer, &shaped, color);
}

static void
draw_text_line(AppState *app, Arena *frame, f32 x, f32 baseline_y, const char *text, RenderColor color)
{
    draw_text_line_font(app, &app->text, frame, x, baseline_y, text, color);
}

static f32
app_measure_text_width(AppState *app, KbTextSystem *font, const char *text)
{
    if (!font || !text || !text[0]) {
        return 0.0f;
    }
    ArenaTemp temp = arena_temp_begin(&app->frame_arena);
    ShapedText shaped = kb_text_shape(&app->frame_arena, font, text, 0.0f, 0.0f);
    arena_temp_end(temp);
    return shaped.width;
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
        }
    }
}

static void
app_draw_query_input(AppState *app, UiDrawList *draw_list, const UiTheme *theme, const UiTextInputControl *input)
{
    f32 input_x = input->bounds.x;
    f32 input_baseline = input->bounds.y + 22.0f;
    f32 input_top = input->bounds.y;
    f32 input_height = input->bounds.h;
    f32 input_right = input->bounds.x + input->bounds.w;
    f32 visible_width = input_right - input_x;
    if (visible_width < 8.0f) {
        visible_width = 8.0f;
    }
    f32 content_height = app->text.line_height;
    if (content_height > input_height - 8.0f) {
        content_height = input_height - 8.0f;
    }
    if (content_height < 16.0f) {
        content_height = 16.0f;
    }
    f32 content_top = input_top + (input_height - content_height) * 0.5f;

    char before_caret[LAUNCHER_MAX_QUERY];
    if (input->caret_index > 0) {
        memcpy(before_caret, input->text, (size_t)input->caret_index);
    }
    before_caret[input->caret_index] = 0;
    ShapedText before_shape = kb_text_shape(&app->frame_arena, &app->text, before_caret, 0.0f, 0.0f);
    f32 caret_x = before_shape.width;

    if (caret_x - app->query_scroll_x > visible_width) {
        app->query_scroll_x = caret_x - visible_width;
    }
    if (caret_x - app->query_scroll_x < 0.0f) {
        app->query_scroll_x = caret_x;
    }
    if (app->query_scroll_x < 0.0f) {
        app->query_scroll_x = 0.0f;
    }

    f32 text_draw_x = input_x - app->query_scroll_x;
    if (input->has_text) {
        ui_draw_text(draw_list, text_draw_x, input_baseline, input->text, theme->fg_primary);
    } else {
        ui_draw_text(draw_list, text_draw_x, input_baseline, input->placeholder, theme->fg_footer);
    }

    if (input->sel_start != input->sel_end) {
        s32 sel_start = input->sel_start;
        s32 sel_end = input->sel_end;
        if (sel_start > sel_end) {
            s32 temp = sel_start;
            sel_start = sel_end;
            sel_end = temp;
        }

        char before_sel[LAUNCHER_MAX_QUERY];
        char selected_text[LAUNCHER_MAX_QUERY];
        if (sel_start > 0) {
            memcpy(before_sel, input->text, (size_t)sel_start);
        }
        before_sel[sel_start] = 0;
        if (sel_end > sel_start) {
            memcpy(selected_text, input->text + sel_start, (size_t)(sel_end - sel_start));
        }
        selected_text[sel_end - sel_start] = 0;

        ShapedText before_sel_shape = kb_text_shape(&app->frame_arena, &app->text, before_sel, 0.0f, 0.0f);
        ShapedText selected_shape = kb_text_shape(&app->frame_arena, &app->text, selected_text, 0.0f, 0.0f);
        f32 sel_x0 = input_x + before_sel_shape.width - app->query_scroll_x;
        f32 sel_x1 = sel_x0 + selected_shape.width;
        if (sel_x1 > input_x && sel_x0 < input_right) {
            f32 clamped_x0 = sel_x0 < input_x ? input_x : sel_x0;
            f32 clamped_x1 = sel_x1 > input_right ? input_right : sel_x1;
            ui_draw_rect(draw_list, ui_rect(clamped_x0, content_top, clamped_x1 - clamped_x0, content_height),
                         theme->input_selection_bg);
            ui_draw_text(draw_list, text_draw_x, input_baseline, input->text, theme->fg_primary);
        }
    }

    if (input->caret_visible) {
        f32 caret_draw_x = input_x + caret_x - app->query_scroll_x;
        if (caret_draw_x >= input_x && caret_draw_x <= input_right) {
            ui_draw_rect(draw_list, ui_rect(caret_draw_x, content_top, 2.0f, content_height), theme->input_caret);
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
    dx11_renderer_begin(&app->renderer, (RenderColor){0.06f, 0.07f, 0.09f, 1.0f});
    UiDrawList draw_list = {0};
    ui_drawlist_begin(&draw_list, &app->frame_arena, 8192);
    UiTheme theme = ui_theme_default();

    const f32 border_thickness = 1.0f;
    const f32 outer_padding = 5.0f;
    const f32 header_gap = 5.0f;
    const f32 footer_gap = 10.0f;
    const f32 footer_h = 16.0f;
    const RenderColor border_color = (RenderColor){0.20f, 0.50f, 0.95f, 1.0f};

    UiRect window_rect = ui_rect(0.0f, 0.0f, (f32)width, (f32)height);
    UiRect content_rect = ui_inset(window_rect, border_thickness, border_thickness, border_thickness, border_thickness);
    UiRect top_bar_rect = ui_rect(content_rect.x + outer_padding, content_rect.y + outer_padding, content_rect.w - outer_padding * 2.0f, 56.0f);
    UiRect footer_rect = ui_rect(content_rect.x + outer_padding + 8.0f,
                                 content_rect.y + content_rect.h - footer_gap - footer_h,
                                 300.0f, footer_h);
    f32 list_top = top_bar_rect.y + top_bar_rect.h + header_gap;
    f32 list_h = footer_rect.y - footer_gap - list_top;
    if (list_h < 0.0f) {
        list_h = 0.0f;
    }
    UiRect list_rect = ui_rect(content_rect.x + outer_padding, list_top, content_rect.w - outer_padding * 2.0f, list_h);
    UiRect list_content_rect = ui_inset(list_rect, 12.0f, 12.0f, 12.0f, 12.0f);
    const char *mode_label = app->mode == SearchMode_Apps ? "Apps" : "Files";
    const f32 top_row_h = 34.0f;
    const f32 top_row_pad_x = 14.0f;
    const f32 top_row_gap = 8.0f;
    const f32 mode_pill_pad_x = 14.0f;
    UiHBoxLayout top_row = ui_hbox_begin(ui_rect(top_bar_rect.x + top_row_pad_x,
                                                 top_bar_rect.y + (top_bar_rect.h - top_row_h) * 0.5f,
                                                 top_bar_rect.w - top_row_pad_x * 2.0f,
                                                 top_row_h),
                                         top_row_gap);
    f32 mode_text_w = app_measure_text_width(app, &app->text, mode_label);
    f32 mode_pill_w = mode_text_w + mode_pill_pad_x * 2.0f;
    if (mode_pill_w < top_row_h) {
        mode_pill_w = top_row_h;
    }
    UiRect mode_pill_rect = ui_hbox_next_fixed(&top_row, mode_pill_w);
    UiTextInputControl input_control = {0};
    input_control.bounds = ui_hbox_next_fill(&top_row, 80.0f);
    input_control.text = app->query;
    input_control.placeholder = "Type to search...";
    input_control.has_text = (app->query_length > 0);
    input_control.scroll_x = app->query_scroll_x;
    input_control.caret_index = app->query_caret;
    input_control.sel_start = app->query_anchor;
    input_control.sel_end = app->query_caret;
    input_control.caret_visible = app->caret_blink_on;

    ui_control_border(&draw_list, window_rect, border_thickness, border_color);
    ui_control_panel(&draw_list, content_rect, theme.bg_window);
    ui_control_panel(&draw_list, top_bar_rect, theme.bg_top_bar);
    ui_control_panel(&draw_list, list_rect, theme.bg_results_panel);
    ui_control_mode_pill(&draw_list, mode_pill_rect, mode_label, mode_text_w, theme.mode_pill_bg, theme.mode_pill_fg, &app->text);
    ui_push_clip_rect(&draw_list, input_control.bounds);
    app_draw_query_input(app, &draw_list, &theme, &input_control);
    ui_pop_clip(&draw_list);

    f32 row_top = list_content_rect.y + 6.0f;
    f32 row_step = app_results_row_step(app);
    f32 row_height = row_step;
    s32 rows_per_page = app_result_rows_per_page(app);
    s32 start_index = app->results_top_index;
    bool show_scrollbar = ((s32)app->results.count > rows_per_page);
    f32 scrollbar_track_w = 8.0f;
    f32 scrollbar_inset = 8.0f;
    f32 scrollbar_reserved_w = show_scrollbar ? (scrollbar_track_w + scrollbar_inset * 2.0f) : 0.0f;
    f32 row_x = list_content_rect.x;
    f32 content_right = list_content_rect.x + list_content_rect.w - scrollbar_reserved_w;
    f32 item_text_x = row_x + 12.0f;
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
        ui_control_results_row(&draw_list, ui_rect(row_x, row_top, content_right - row_x, list_control.row_height), selected, theme.row_selected_bg);

        const LaunchItem *item = app->results.items[(u32)i].item;
        f32 title_baseline = row_top + app->text_results.pixel_height + 2.0f;
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
                          list_content_rect.x + 5.0f,
                          list_content_rect.y + 5.0f + app->text_results.pixel_height,
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

    char diagnostics[128];
    _snprintf_s(diagnostics, sizeof(diagnostics), _TRUNCATE, "Mode: %s | Results: %u",
        app->mode == SearchMode_Apps ? "Apps" : "Files",
        app->results.count);
    ui_control_footer(&draw_list, footer_rect, diagnostics, theme.fg_footer, &app->text_results);

    app_flush_ui_draw_list(app, &draw_list, width, height);
    dx11_renderer_end(&app->renderer);
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
    case WM_MOUSEWHEEL:
        if (app && app->visible) {
            s16 wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (wheel_delta != 0) {
                app_move_selection(app, wheel_delta > 0 ? -1 : 1);
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

    s32 initial_width = 920;
    s32 initial_height = 560;
    app_window_size_for_rows(app, &initial_width, &initial_height);
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
    debug_log_wide(L"renderer initialized");

    wchar_t font_path[MAX_PATH];
    _snwprintf_s(font_path, array_count(font_path), _TRUNCATE, L"%ls\\Fonts\\CascadiaMono.ttf", _wgetenv(L"WINDIR"));
    if (!kb_text_init(&app->text, font_path, 24.0f)) {
        debug_log_wide(L"kb_text_init failed font=%ls", font_path);
        return false;
    }
    debug_log_wide(L"text initialized");
    dx11_renderer_upload_atlas(&app->renderer, &app->text.raster, 0);
    app->text.raster.atlas_dirty = false;

    if (!kb_text_init(&app->text_results, font_path, LAUNCHER_RESULTS_FONT_PX)) {
        debug_log_wide(L"kb_text_init failed (results) font=%ls", font_path);
        return false;
    }
    dx11_renderer_upload_atlas(&app->renderer, &app->text_results.raster, 1);
    app->text_results.raster.atlas_dirty = false;

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
    kb_text_shutdown(&app->text_results);
    kb_text_shutdown(&app->text);
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

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        fatal_message(L"Failed to initialize COM.");
    }

    AppState app;
    g_app = &app;
    if (!app_init(&app, instance)) {
        fatal_message(L"Failed to initialize launcher.");
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app_shutdown(&app);
    CoUninitialize();
    return 0;
}
