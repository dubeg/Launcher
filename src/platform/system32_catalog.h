#ifndef LAUNCHER_SYSTEM32_CATALOG_H
#define LAUNCHER_SYSTEM32_CATALOG_H

#include "../app/app.h"
#include "catalog_aliases.h"

/* Indexes *.exe, *.msc, *.cpl under %WINDIR% (recursive); skips WinSxS, DriverStore, etc. */
bool system32_catalog_build(Arena *arena, const CatalogAliases *aliases, LaunchItemArray *out_items);

#endif
