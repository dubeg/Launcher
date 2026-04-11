#ifndef LAUNCHER_CATALOG_H
#define LAUNCHER_CATALOG_H

#include "../app/app.h"
#include "catalog_aliases.h"

bool app_catalog_build(Arena *arena, const wchar_t *install_dir, LaunchItemArray *out_items, CatalogAliases *out_aliases);

#endif
