#ifndef LAUNCHER_START_MENU_SCAN_H
#define LAUNCHER_START_MENU_SCAN_H

#include "../app/app.h"
#include "catalog_aliases.h"

bool start_menu_scan_build(Arena *arena, const CatalogAliases *aliases, LaunchItemArray *out_items);

#endif
