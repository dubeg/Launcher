#ifndef LAUNCHER_CATALOG_H
#define LAUNCHER_CATALOG_H

#include "../app/app.h"
#include "catalog_aliases.h"

bool catalog_resolve_install_relative(const wchar_t *start_dir, const wchar_t *relative_path, wchar_t *out, size_t out_cap_chars);

typedef struct CatalogWatchRootDirs {
    wchar_t **paths;
    u32 count;
} CatalogWatchRootDirs;

bool catalog_watch_root_dirs_collect(const wchar_t *install_dir, CatalogWatchRootDirs *out);
void catalog_watch_root_dirs_free(CatalogWatchRootDirs *dirs);

bool app_catalog_build(Arena *arena, const wchar_t *install_dir, LaunchItemArray *out_items, CatalogAliases *out_aliases);

#endif
