#ifndef LAUNCHER_FONT_RASTER_H
#define LAUNCHER_FONT_RASTER_H

#include "../core/base.h"

struct stbtt_fontinfo;

typedef struct FontGlyph {
    u16 glyph_id;
    s16 xoff;
    s16 yoff;
    u16 width;
    u16 height;
    u16 atlas_x;
    u16 atlas_y;
    f32 u0;
    f32 v0;
    f32 u1;
    f32 v1;
} FontGlyph;

typedef struct FontRaster {
    FileData font_file;
    struct stbtt_fontinfo *info;
    f32 pixel_height;
    f32 scale;
    int atlas_width;
    int atlas_height;
    int atlas_cursor_x;
    int atlas_cursor_y;
    int atlas_row_height;
    u8 *atlas_pixels;
    FontGlyph *glyphs;
    u32 glyph_count;
    u32 glyph_capacity;
    bool atlas_dirty;
} FontRaster;

bool font_raster_init(FontRaster *raster, const wchar_t *font_path, f32 pixel_height);
void font_raster_shutdown(FontRaster *raster);
const FontGlyph *font_raster_get_glyph(FontRaster *raster, u16 glyph_id);

#endif
