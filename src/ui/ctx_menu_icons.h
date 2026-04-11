#ifndef LAUNCHER_CTX_MENU_ICONS_H
#define LAUNCHER_CTX_MENU_ICONS_H

#include "../render/dx11_renderer.h"

/* Rasterized from data/icons/*.svg via NanoSVG (third_party/nanosvg). */

#define CTX_MENU_ICON_RASTER_PX 48
#define CTX_MENU_ICON_COUNT 3

bool ctx_menu_icons_load(Dx11Renderer *renderer, const wchar_t *install_dir, Dx11Texture out_icons[CTX_MENU_ICON_COUNT]);
void ctx_menu_icons_destroy(Dx11Texture icons[CTX_MENU_ICON_COUNT]);

#endif
