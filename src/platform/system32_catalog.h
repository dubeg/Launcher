#ifndef LAUNCHER_SYSTEM32_CATALOG_H
#define LAUNCHER_SYSTEM32_CATALOG_H

#include "../app/app.h"
#include "catalog_aliases.h"

bool system32_catalog_build(Arena *arena, const CatalogAliases *aliases, LaunchItemArray *out_items);

#endif
