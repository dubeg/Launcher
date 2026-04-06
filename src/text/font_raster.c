#include "font_raster.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

#include <string.h>

static FontGlyph *
font_raster_find_glyph(FontRaster *raster, u16 glyph_id)
{
    for (u32 i = 0; i < raster->glyph_count; ++i) {
        if (raster->glyphs[i].glyph_id == glyph_id) {
            return &raster->glyphs[i];
        }
    }
    return NULL;
}

static FontGlyph *
font_raster_push_glyph(FontRaster *raster)
{
    if (raster->glyph_count >= raster->glyph_capacity) {
        u32 new_capacity = raster->glyph_capacity ? raster->glyph_capacity * 2 : 256;
        raster->glyphs = (FontGlyph *)heap_realloc(raster->glyphs, sizeof(FontGlyph) * new_capacity);
        if (!raster->glyphs) {
            return NULL;
        }
        ZeroMemory(raster->glyphs + raster->glyph_capacity, sizeof(FontGlyph) * (new_capacity - raster->glyph_capacity));
        raster->glyph_capacity = new_capacity;
    }
    return &raster->glyphs[raster->glyph_count++];
}

bool
font_raster_init(FontRaster *raster, const wchar_t *font_path, f32 pixel_height)
{
    ZeroMemory(raster, sizeof(*raster));
    raster->pixel_height = pixel_height;
    raster->atlas_width = 2048;
    raster->atlas_height = 2048;
    raster->atlas_pixels = (u8 *)heap_alloc_zero((size_t)raster->atlas_width * (size_t)raster->atlas_height * 4);
    raster->info = (stbtt_fontinfo *)heap_alloc_zero(sizeof(stbtt_fontinfo));
    if (!raster->atlas_pixels || !raster->info) {
        return false;
    }

    raster->font_file = read_entire_file_wide(font_path);
    if (!raster->font_file.data || !stbtt_InitFont(raster->info, (const unsigned char *)raster->font_file.data, stbtt_GetFontOffsetForIndex((const unsigned char *)raster->font_file.data, 0))) {
        return false;
    }

    raster->scale = stbtt_ScaleForPixelHeight(raster->info, pixel_height);
    raster->atlas_cursor_x = 1;
    raster->atlas_cursor_y = 1;
    raster->atlas_row_height = 0;
    raster->atlas_dirty = true;
    return true;
}

void
font_raster_shutdown(FontRaster *raster)
{
    if (!raster) {
        return;
    }
    free_file_data(&raster->font_file);
    heap_free(raster->atlas_pixels);
    heap_free(raster->glyphs);
    heap_free(raster->info);
    ZeroMemory(raster, sizeof(*raster));
}

const FontGlyph *
font_raster_get_glyph(FontRaster *raster, u16 glyph_id)
{
    FontGlyph *existing = font_raster_find_glyph(raster, glyph_id);
    if (existing) {
        return existing;
    }

    int width = 0;
    int height = 0;
    int xoff = 0;
    int yoff = 0;
    unsigned char *bitmap = stbtt_GetGlyphBitmap(raster->info, raster->scale, raster->scale, glyph_id, &width, &height, &xoff, &yoff);

    int padding = 2;
    if (raster->atlas_cursor_x + width + padding >= raster->atlas_width) {
        raster->atlas_cursor_x = 1;
        raster->atlas_cursor_y += raster->atlas_row_height + padding;
        raster->atlas_row_height = 0;
    }
    if (raster->atlas_cursor_y + height + padding >= raster->atlas_height) {
        if (bitmap) {
            stbtt_FreeBitmap(bitmap, NULL);
        }
        return NULL;
    }

    FontGlyph *glyph = font_raster_push_glyph(raster);
    if (!glyph) {
        if (bitmap) {
            stbtt_FreeBitmap(bitmap, NULL);
        }
        return NULL;
    }

    glyph->glyph_id = glyph_id;
    glyph->xoff = (s16)xoff;
    glyph->yoff = (s16)yoff;
    glyph->width = (u16)width;
    glyph->height = (u16)height;
    glyph->atlas_x = (u16)raster->atlas_cursor_x;
    glyph->atlas_y = (u16)raster->atlas_cursor_y;

    if (bitmap && width > 0 && height > 0) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t src_index = (size_t)y * (size_t)width + (size_t)x;
                size_t dst_index = ((size_t)(glyph->atlas_y + y) * (size_t)raster->atlas_width + (size_t)(glyph->atlas_x + x)) * 4;
                u8 alpha = bitmap[src_index];
                raster->atlas_pixels[dst_index + 0] = 255;
                raster->atlas_pixels[dst_index + 1] = 255;
                raster->atlas_pixels[dst_index + 2] = 255;
                raster->atlas_pixels[dst_index + 3] = alpha;
            }
        }
    }

    glyph->u0 = (f32)glyph->atlas_x / (f32)raster->atlas_width;
    glyph->v0 = (f32)glyph->atlas_y / (f32)raster->atlas_height;
    glyph->u1 = (f32)(glyph->atlas_x + glyph->width) / (f32)raster->atlas_width;
    glyph->v1 = (f32)(glyph->atlas_y + glyph->height) / (f32)raster->atlas_height;

    raster->atlas_cursor_x += width + padding;
    if (height > raster->atlas_row_height) {
        raster->atlas_row_height = height;
    }
    raster->atlas_dirty = true;

    if (bitmap) {
        stbtt_FreeBitmap(bitmap, NULL);
    }

    return glyph;
}
