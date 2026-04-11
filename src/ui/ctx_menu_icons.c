#include "ctx_menu_icons.h"

#include "../core/base.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

static bool
rasterize_svg_file(Dx11Renderer *renderer, const wchar_t *svg_path, Dx11Texture *out)
{
    FileData fd = read_entire_file_wide(svg_path);
    if (!fd.data || fd.size == 0) {
        return false;
    }

    char *mutable_copy = (char *)heap_alloc_zero(fd.size + 1);
    if (!mutable_copy) {
        free_file_data(&fd);
        return false;
    }
    memcpy(mutable_copy, fd.data, fd.size);
    free_file_data(&fd);

    NSVGimage *image = nsvgParse(mutable_copy, "px", 96.0f);
    if (!image) {
        heap_free(mutable_copy);
        return false;
    }

    float iw = image->width;
    float ih = image->height;
    if (iw < 1.0f) {
        iw = 24.0f;
    }
    if (ih < 1.0f) {
        ih = 24.0f;
    }

    int dim = CTX_MENU_ICON_RASTER_PX;
    float sc_w = (float)dim / iw;
    float sc_h = (float)dim / ih;
    float sc = sc_w < sc_h ? sc_w : sc_h;
    float tx = ((float)dim - iw * sc) * 0.5f;
    float ty = ((float)dim - ih * sc) * 0.5f;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        heap_free(mutable_copy);
        return false;
    }

    size_t pix_bytes = (size_t)dim * (size_t)dim * 4;
    unsigned char *pixels = (unsigned char *)heap_alloc_zero(pix_bytes);
    if (!pixels) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        heap_free(mutable_copy);
        return false;
    }

    nsvgRasterize(rast, image, tx, ty, sc, pixels, dim, dim, dim * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    heap_free(mutable_copy);

    bool ok = dx11_renderer_create_texture_rgba(renderer, dim, dim, pixels, out);
    heap_free(pixels);
    return ok;
}

bool
ctx_menu_icons_load(Dx11Renderer *renderer, const wchar_t *install_dir, Dx11Texture out_icons[CTX_MENU_ICON_COUNT])
{
    if (!renderer || !install_dir || !out_icons) {
        return false;
    }
    ZeroMemory(out_icons, sizeof(Dx11Texture) * CTX_MENU_ICON_COUNT);

    wchar_t path[MAX_PATH * 2];
    static const wchar_t *names[CTX_MENU_ICON_COUNT] = {
        L"data\\icons\\ctx_launch.svg",
        L"data\\icons\\ctx_copy_path.svg",
        L"data\\icons\\ctx_open_location.svg",
    };

    for (u32 i = 0; i < CTX_MENU_ICON_COUNT; ++i) {
        if (i == 0) {
            /* Launch row has no icon; skip rasterizing ctx_launch.svg */
            continue;
        }
        _snwprintf_s(path, array_count(path), _TRUNCATE, L"%ls\\%ls", install_dir, names[i]);
        if (!rasterize_svg_file(renderer, path, &out_icons[i])) {
            ctx_menu_icons_destroy(out_icons);
            return false;
        }
    }
    return true;
}

void
ctx_menu_icons_destroy(Dx11Texture icons[CTX_MENU_ICON_COUNT])
{
    if (!icons) {
        return;
    }
    for (u32 i = 0; i < CTX_MENU_ICON_COUNT; ++i) {
        dx11_renderer_destroy_texture(&icons[i]);
    }
}
