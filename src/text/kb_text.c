#include "kb_text.h"

#define KB_TEXT_SHAPE_IMPLEMENTATION
#include "../../third_party/kb/kb_text_shape.h"

bool
kb_text_init(KbTextSystem *text, const wchar_t *font_path, f32 pixel_height)
{
    ZeroMemory(text, sizeof(*text));
    if (!font_raster_init(&text->raster, font_path, pixel_height)) {
        return false;
    }

    text->shape_context = kbts_CreateShapeContext(0, 0);
    if (!text->shape_context) {
        return false;
    }

    char font_path_utf8[MAX_PATH * 4];
    utf8_from_wide_buffer(font_path, font_path_utf8, array_count(font_path_utf8));
    text->kb_font = kbts_ShapePushFontFromFile(text->shape_context, font_path_utf8, 0);
    if (!text->kb_font || text->kb_font->Error != KBTS_LOAD_FONT_ERROR_NONE) {
        return false;
    }
    text->kb_font->UserData = &text->raster;
    text->pixel_height = pixel_height;

    kbts_font_info2_1 info = {0};
    info.Base.Size = sizeof(info);
    kbts_GetFontInfo2(text->kb_font, (kbts_font_info2 *)&info);
    text->scale = (f32)pixel_height / (f32)info.UnitsPerEm;
    text->ascent = (f32)info.Ascent * text->scale;
    text->descent = (f32)info.Descent * text->scale;
    text->line_height = (f32)(info.Ascent - info.Descent + info.LineGap) * text->scale;
    if (text->line_height < pixel_height) {
        text->line_height = pixel_height * 1.2f;
    }
    return true;
}

void
kb_text_shutdown(KbTextSystem *text)
{
    if (!text) {
        return;
    }
    if (text->shape_context) {
        kbts_DestroyShapeContext(text->shape_context);
    }
    font_raster_shutdown(&text->raster);
    ZeroMemory(text, sizeof(*text));
}

ShapedText
kb_text_shape(Arena *arena, KbTextSystem *text, const char *utf8, f32 x, f32 baseline_y)
{
    ShapedText shaped = {0};
    if (!utf8 || !utf8[0]) {
        shaped.line_height = text->line_height;
        return shaped;
    }

    kbts_ShapeBegin(text->shape_context, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);
    kbts_ShapeUtf8(text->shape_context, utf8, (int)strlen(utf8), KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
    kbts_ShapeEnd(text->shape_context);

    u32 capacity = (u32)strlen(utf8) * 4 + 16;
    shaped.quads = (TextQuad *)arena_push_zero(arena, sizeof(TextQuad) * capacity, sizeof(void *));
    shaped.line_height = text->line_height;

    f32 cursor_x = x;
    kbts_run run;
    while (kbts_ShapeRun(text->shape_context, &run)) {
        kbts_glyph *glyph = 0;
        while (kbts_GlyphIteratorNext(&run.Glyphs, &glyph)) {
            const FontGlyph *font_glyph = font_raster_get_glyph(&text->raster, glyph->Id);
            if (!font_glyph) {
                cursor_x += (f32)glyph->AdvanceX * text->scale;
                continue;
            }

            if (shaped.count >= capacity) {
                break;
            }

            TextQuad *quad = &shaped.quads[shaped.count++];
            quad->x0 = cursor_x + (f32)glyph->OffsetX * text->scale + (f32)font_glyph->xoff;
            quad->y0 = baseline_y + (f32)glyph->OffsetY * text->scale + (f32)font_glyph->yoff;
            quad->x1 = quad->x0 + (f32)font_glyph->width;
            quad->y1 = quad->y0 + (f32)font_glyph->height;
            quad->u0 = font_glyph->u0;
            quad->v0 = font_glyph->v0;
            quad->u1 = font_glyph->u1;
            quad->v1 = font_glyph->v1;

            cursor_x += (f32)glyph->AdvanceX * text->scale;
        }
    }

    shaped.width = cursor_x - x;
    return shaped;
}
