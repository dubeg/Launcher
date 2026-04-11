#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include "../core/base.h"
#include "../render/dx11_renderer.h"

typedef struct UiRect {
    f32 x;
    f32 y;
    f32 w;
    f32 h;
} UiRect;

typedef enum UiDrawCmdType {
    UiDrawCmdType_Rect = 0,
    UiDrawCmdType_Text = 1,
    UiDrawCmdType_ClipPush = 2,
    UiDrawCmdType_ClipPop = 3,
    UiDrawCmdType_Image = 4,
} UiDrawCmdType;

typedef struct UiDrawCmdRect {
    UiRect rect;
    RenderColor color;
} UiDrawCmdRect;

typedef struct UiDrawCmdText {
    f32 x;
    f32 baseline_y;
    const char *text;
    RenderColor color;
    f32 max_width;
    struct KbTextSystem *font;
} UiDrawCmdText;

typedef struct UiDrawCmdClip {
    UiRect rect;
} UiDrawCmdClip;

typedef struct UiDrawCmdImage {
    UiRect rect;
    void *texture_srv;
    RenderColor tint;
} UiDrawCmdImage;

typedef struct UiDrawCmd {
    UiDrawCmdType type;
    union {
        UiDrawCmdRect rect;
        UiDrawCmdText text;
        UiDrawCmdClip clip;
        UiDrawCmdImage image;
    };
} UiDrawCmd;

typedef struct UiDrawList {
    UiDrawCmd *commands;
    u32 count;
    u32 capacity;
} UiDrawList;

typedef struct UiTheme {
    RenderColor bg_window;
    RenderColor bg_top_bar;
    RenderColor bg_results_panel;
    RenderColor fg_primary;
    RenderColor fg_secondary;
    RenderColor fg_footer;
    RenderColor mode_pill_bg;
    RenderColor mode_pill_fg;
    RenderColor row_selected_bg;
    RenderColor row_hover_bg;
    RenderColor input_selection_bg;
    RenderColor input_caret;
    RenderColor scrollbar_track;
    RenderColor scrollbar_thumb;
} UiTheme;

typedef struct UiTextInputControl {
    UiRect bounds;
    const char *text;
    const char *placeholder;
    bool has_text;
    f32 *scroll_x;
    s32 caret_index;
    s32 sel_start;
    s32 sel_end;
    bool caret_visible;
} UiTextInputControl;

typedef struct UiResultsListControl {
    UiRect bounds;
    UiRect content_bounds;
    s32 selected_index;
    s32 start_index;
    s32 end_index;
    s32 rows_per_page;
    s32 total_count;
    bool show_scrollbar;
    f32 row_height;
    f32 row_step;
} UiResultsListControl;

typedef struct UiScrollbarControl {
    UiRect bounds;
    bool visible;
    f32 track_width;
    f32 inset;
    s32 total_items;
    s32 visible_items;
    s32 top_index;
} UiScrollbarControl;

typedef struct UiHBoxLayout {
    UiRect bounds;
    f32 cursor_x;
    f32 gap;
} UiHBoxLayout;

UiTheme ui_theme_default(void);
void ui_drawlist_begin(UiDrawList *list, Arena *arena, u32 capacity);
bool ui_draw_rect(UiDrawList *list, UiRect rect, RenderColor color);
bool ui_draw_text(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color);
bool ui_draw_text_font(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color, struct KbTextSystem *font);
bool ui_draw_text_font_clamped(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color, struct KbTextSystem *font,
                               f32 max_width);
bool ui_draw_text_clamped(UiDrawList *list, f32 x, f32 baseline_y, const char *text, RenderColor color, f32 max_width);
bool ui_draw_image(UiDrawList *list, UiRect rect, void *texture_srv, RenderColor tint);

UiRect ui_rect(f32 x, f32 y, f32 w, f32 h);
UiRect ui_inset(UiRect rect, f32 inset_left, f32 inset_top, f32 inset_right, f32 inset_bottom);
UiRect ui_rect_intersect(UiRect a, UiRect b);
void ui_rect_to_scissor_pixels(UiRect rect, u32 window_w, u32 window_h, u32 *out_left, u32 *out_top, u32 *out_right, u32 *out_bottom);
bool ui_push_clip_rect(UiDrawList *list, UiRect rect);
bool ui_pop_clip(UiDrawList *list);

void ui_control_panel(UiDrawList *list, UiRect bounds, RenderColor color);
void ui_control_border(UiDrawList *list, UiRect bounds, f32 thickness, RenderColor color);
void ui_control_mode_pill(UiDrawList *list, UiRect bounds, const char *label, f32 label_width, RenderColor bg, RenderColor fg, struct KbTextSystem *font);
void ui_control_text_input(UiDrawList *list,
                           Arena *arena,
                           const UiTextInputControl *input,
                           const UiTheme *theme,
                           struct KbTextSystem *font,
                           f32 dpi_scale);
void ui_control_footer(UiDrawList *list, UiRect bounds, const char *text, RenderColor color, struct KbTextSystem *font);
void ui_control_results_row(UiDrawList *list, UiRect bounds, bool selected, RenderColor selected_bg);
void ui_control_scrollbar(UiDrawList *list, const UiScrollbarControl *scrollbar, RenderColor track_color, RenderColor thumb_color);

void ui_control_context_menu_panel(UiDrawList *list, UiRect bounds, RenderColor fill, RenderColor border_color, f32 border_thickness);
void ui_control_context_menu_item(UiDrawList *list,
                                  Arena *arena,
                                  const UiTheme *theme,
                                  struct KbTextSystem *font,
                                  f32 dpi_scale,
                                  UiRect row,
                                  bool selected,
                                  bool hover,
                                  bool enabled,
                                  const char *text,
                                  void *icon_texture_srv);

f32 ui_shortcut_lnk_badge_chip_width(Arena *arena, struct KbTextSystem *font, f32 dpi_scale, const char *label);
UiRect ui_shortcut_lnk_badge_bounds(Arena *arena, struct KbTextSystem *font, f32 right_edge_x, f32 row_top, f32 row_height, f32 dpi_scale,
                                    const char *label);
void ui_control_shortcut_lnk_badge(UiDrawList *list, Arena *arena, struct KbTextSystem *font, f32 right_edge_x, f32 row_top, f32 row_height,
                                   f32 dpi_scale, const char *label, bool hover, RenderColor chip_bg, RenderColor label_fg,
                                   RenderColor chip_bg_hover, RenderColor label_fg_hover);
UiHBoxLayout ui_hbox_begin(UiRect bounds, f32 gap);
UiRect ui_hbox_next_fixed(UiHBoxLayout *layout, f32 width);
UiRect ui_hbox_next_fill(UiHBoxLayout *layout, f32 min_width);

#endif
