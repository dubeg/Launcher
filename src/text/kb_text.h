#ifndef LAUNCHER_KB_TEXT_H
#define LAUNCHER_KB_TEXT_H

#include "../core/base.h"
#include "font_raster.h"

typedef struct TextQuad {
    f32 x0;
    f32 y0;
    f32 x1;
    f32 y1;
    f32 u0;
    f32 v0;
    f32 u1;
    f32 v1;
} TextQuad;

typedef struct ShapedText {
    TextQuad *quads;
    u32 count;
    f32 width;
    f32 line_height;
} ShapedText;

typedef struct KbTextSystem {
    struct kbts_shape_context *shape_context;
    struct kbts_font *kb_font;
    FontRaster raster;
    f32 pixel_height;
    f32 scale;
    f32 line_height;
    f32 ascent;
    f32 descent;
} KbTextSystem;

bool kb_text_init(KbTextSystem *text, const wchar_t *font_path, f32 pixel_height);
void kb_text_shutdown(KbTextSystem *text);
ShapedText kb_text_shape(Arena *arena, KbTextSystem *text, const char *utf8, f32 x, f32 baseline_y);

#endif
