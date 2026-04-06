#include "ui.h"
#include "../text/kb_text.h"

#include <math.h>

UiTheme
ui_theme_default(void)
{
    UiTheme theme = {0};
    theme.bg_window = (RenderColor){0.06f, 0.07f, 0.09f, 1.0f};
    theme.bg_top_bar = (RenderColor){0.11f, 0.12f, 0.15f, 1.0f};
    theme.bg_results_panel = (RenderColor){0.09f, 0.10f, 0.13f, 1.0f};
    theme.fg_primary = (RenderColor){0.97f, 0.98f, 1.0f, 1.0f};
    theme.fg_secondary = (RenderColor){0.60f, 0.65f, 0.72f, 1.0f};
    theme.fg_footer = (RenderColor){0.56f, 0.60f, 0.66f, 1.0f};
    theme.mode_pill_bg = (RenderColor){0.22f, 0.45f, 0.82f, 1.0f};
    theme.mode_pill_fg = (RenderColor){0.95f, 0.96f, 0.98f, 1.0f};
    theme.row_selected_bg = (RenderColor){0.16f, 0.25f, 0.42f, 1.0f};
    theme.input_selection_bg = (RenderColor){0.24f, 0.43f, 0.75f, 0.35f};
    theme.input_caret = (RenderColor){0.97f, 0.98f, 1.0f, 1.0f};
    theme.scrollbar_track = (RenderColor){0.18f, 0.20f, 0.25f, 0.9f};
    theme.scrollbar_thumb = (RenderColor){0.40f, 0.46f, 0.58f, 0.95f};
    return theme;
}

void
ui_drawlist_begin(UiDrawList *list, Arena *arena, u32 capacity)
{
    list->commands = (UiDrawCmd *)arena_push_zero(arena, sizeof(UiDrawCmd) * capacity, sizeof(void *));
    list->count = 0;
    list->capacity = capacity;
}

bool
ui_draw_rect(UiDrawList *list, UiRect rect, RenderColor color)
{
    if (!list || list->count >= list->capacity) {
        return false;
    }
    UiDrawCmd *cmd = &list->commands[list->count++];
    cmd->type = UiDrawCmdType_Rect;
    cmd->rect.rect = rect;
    cmd->rect.color = color;
    return true;
}

bool
ui_draw_text(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color)
{
    return ui_draw_text_font(list, x, baseline_y, text, color, NULL);
}

bool
ui_draw_text_font(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color, KbTextSystem *font)
{
    if (!list || list->count >= list->capacity) {
        return false;
    }
    UiDrawCmd *cmd = &list->commands[list->count++];
    cmd->type = UiDrawCmdType_Text;
    cmd->text.x = x;
    cmd->text.baseline_y = baseline_y;
    cmd->text.text = text;
    cmd->text.color = color;
    cmd->text.max_width = -1.0f;
    cmd->text.font = font;
    return true;
}

bool
ui_draw_text_clamped(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color, f32 max_width)
{
    if (!list || list->count >= list->capacity) {
        return false;
    }
    UiDrawCmd *cmd = &list->commands[list->count++];
    cmd->type = UiDrawCmdType_Text;
    cmd->text.x = x;
    cmd->text.baseline_y = baseline_y;
    cmd->text.text = text;
    cmd->text.color = color;
    cmd->text.max_width = max_width;
    cmd->text.font = NULL;
    return true;
}

bool
ui_draw_image(UiDrawList *list, UiRect rect, void *texture_srv, RenderColor tint)
{
    if (!list || list->count >= list->capacity || !texture_srv) {
        return false;
    }
    UiDrawCmd *cmd = &list->commands[list->count++];
    cmd->type = UiDrawCmdType_Image;
    cmd->image.rect = rect;
    cmd->image.texture_srv = texture_srv;
    cmd->image.tint = tint;
    return true;
}

UiRect
ui_rect(f32 x, f32 y, f32 w, f32 h)
{
    UiRect rect = {x, y, w, h};
    return rect;
}

UiRect
ui_rect_intersect(UiRect a, UiRect b)
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    f32 y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    f32 w = x1 - x0;
    f32 h = y1 - y0;
    if (w <= 0.0f || h <= 0.0f) {
        return ui_rect(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return ui_rect(x0, y0, w, h);
}

void
ui_rect_to_scissor_pixels(UiRect rect, u32 window_w, u32 window_h, u32 *out_left, u32 *out_top, u32 *out_right, u32 *out_bottom)
{
    f32 x1 = rect.x + rect.w;
    f32 y1 = rect.y + rect.h;
    s32 left = (s32)floorf(rect.x);
    s32 top = (s32)floorf(rect.y);
    s32 right = (s32)ceilf(x1);
    s32 bottom = (s32)ceilf(y1);
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > (s32)window_w) {
        right = (s32)window_w;
    }
    if (bottom > (s32)window_h) {
        bottom = (s32)window_h;
    }
    if (right < left) {
        right = left;
    }
    if (bottom < top) {
        bottom = top;
    }
    *out_left = (u32)left;
    *out_top = (u32)top;
    *out_right = (u32)right;
    *out_bottom = (u32)bottom;
}

UiRect
ui_inset(UiRect rect, f32 inset_left, f32 inset_top, f32 inset_right, f32 inset_bottom)
{
    rect.x += inset_left;
    rect.y += inset_top;
    rect.w -= (inset_left + inset_right);
    rect.h -= (inset_top + inset_bottom);
    if (rect.w < 0.0f) {
        rect.w = 0.0f;
    }
    if (rect.h < 0.0f) {
        rect.h = 0.0f;
    }
    return rect;
}

bool
ui_push_clip_rect(UiDrawList *list, UiRect rect)
{
    if (!list || list->count >= list->capacity) {
        return false;
    }
    UiDrawCmd *cmd = &list->commands[list->count++];
    cmd->type = UiDrawCmdType_ClipPush;
    cmd->clip.rect = rect;
    return true;
}

bool
ui_pop_clip(UiDrawList *list)
{
    if (!list || list->count >= list->capacity) {
        return false;
    }
    UiDrawCmd *cmd = &list->commands[list->count++];
    cmd->type = UiDrawCmdType_ClipPop;
    return true;
}

void
ui_control_panel(UiDrawList *list, UiRect bounds, RenderColor color)
{
    ui_draw_rect(list, bounds, color);
}

void
ui_control_border(UiDrawList *list, UiRect bounds, f32 thickness, RenderColor color)
{
    if (thickness <= 0.0f || bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return;
    }
    if (thickness * 2.0f > bounds.w) {
        thickness = bounds.w * 0.5f;
    }
    if (thickness * 2.0f > bounds.h) {
        thickness = bounds.h * 0.5f;
    }

    ui_draw_rect(list, ui_rect(bounds.x, bounds.y, bounds.w, thickness), color);
    ui_draw_rect(list, ui_rect(bounds.x, bounds.y + bounds.h - thickness, bounds.w, thickness), color);
    ui_draw_rect(list, ui_rect(bounds.x, bounds.y + thickness, thickness, bounds.h - thickness * 2.0f), color);
    ui_draw_rect(list, ui_rect(bounds.x + bounds.w - thickness, bounds.y + thickness, thickness, bounds.h - thickness * 2.0f), color);
}

void
ui_control_mode_pill(UiDrawList *list, UiRect bounds, const char *label, f32 label_width, RenderColor bg, RenderColor fg, KbTextSystem *font)
{
    ui_draw_rect(list, bounds, bg);

    if (label && label[0]) {
        f32 text_x = bounds.x;
        f32 baseline = bounds.y + 22.0f;
        if (font) {
            if (label_width < 0.0f) {
                label_width = 0.0f;
            }
            text_x = bounds.x + (bounds.w - label_width) * 0.5f;
            baseline = bounds.y + bounds.h * 0.5f + (font->ascent + font->descent) * 0.5f;
        }
        ui_draw_text_font(list, text_x, baseline, label, fg, font);
    }
}

void
ui_control_footer(UiDrawList *list, UiRect bounds, const char *text, RenderColor color, KbTextSystem *font)
{
    ui_draw_text_font(list, bounds.x, bounds.y + bounds.h, text, color, font);
}

void
ui_control_results_row(UiDrawList *list, UiRect bounds, bool selected, RenderColor selected_bg)
{
    if (selected) {
        ui_draw_rect(list, bounds, selected_bg);
    }
}

void
ui_control_scrollbar(UiDrawList *list, const UiScrollbarControl *scrollbar, RenderColor track_color, RenderColor thumb_color)
{
    if (!scrollbar || !scrollbar->visible) {
        return;
    }

    f32 track_x = scrollbar->bounds.x + scrollbar->bounds.w - scrollbar->inset - scrollbar->track_width;
    f32 track_y = scrollbar->bounds.y + scrollbar->inset;
    f32 track_h = scrollbar->bounds.h - scrollbar->inset * 2.0f;
    if (track_h < 20.0f) {
        track_h = 20.0f;
    }
    ui_draw_rect(list, ui_rect(track_x, track_y, scrollbar->track_width, track_h), track_color);

    f32 visible_ratio = 1.0f;
    if (scrollbar->total_items > 0) {
        visible_ratio = (f32)scrollbar->visible_items / (f32)scrollbar->total_items;
    }
    if (visible_ratio > 1.0f) {
        visible_ratio = 1.0f;
    }
    if (visible_ratio < 0.0f) {
        visible_ratio = 0.0f;
    }

    f32 thumb_h = track_h * visible_ratio;
    if (thumb_h < 24.0f) {
        thumb_h = 24.0f;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }

    s32 scroll_range = scrollbar->total_items - scrollbar->visible_items;
    f32 scroll_t = 0.0f;
    if (scroll_range > 0) {
        scroll_t = (f32)scrollbar->top_index / (f32)scroll_range;
    }
    if (scroll_t < 0.0f) {
        scroll_t = 0.0f;
    }
    if (scroll_t > 1.0f) {
        scroll_t = 1.0f;
    }

    f32 thumb_y = track_y + (track_h - thumb_h) * scroll_t;
    ui_draw_rect(list, ui_rect(track_x, thumb_y, scrollbar->track_width, thumb_h), thumb_color);
}

UiHBoxLayout
ui_hbox_begin(UiRect bounds, f32 gap)
{
    UiHBoxLayout layout = {0};
    layout.bounds = bounds;
    layout.cursor_x = bounds.x;
    layout.gap = gap;
    return layout;
}

UiRect
ui_hbox_next_fixed(UiHBoxLayout *layout, f32 width)
{
    if (!layout) {
        return ui_rect(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (width < 0.0f) {
        width = 0.0f;
    }
    f32 max_w = layout->bounds.x + layout->bounds.w - layout->cursor_x;
    if (width > max_w) {
        width = max_w;
    }
    UiRect rect = ui_rect(layout->cursor_x, layout->bounds.y, width, layout->bounds.h);
    layout->cursor_x += width + layout->gap;
    return rect;
}

UiRect
ui_hbox_next_fill(UiHBoxLayout *layout, f32 min_width)
{
    if (!layout) {
        return ui_rect(0.0f, 0.0f, 0.0f, 0.0f);
    }
    f32 width = layout->bounds.x + layout->bounds.w - layout->cursor_x;
    if (width < 0.0f) {
        width = 0.0f;
    }
    if (width < min_width) {
        width = min_width;
    }
    UiRect rect = ui_rect(layout->cursor_x, layout->bounds.y, width, layout->bounds.h);
    layout->cursor_x += width + layout->gap;
    return rect;
}
