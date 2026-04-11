#ifndef LAUNCHER_CATALOG_ALIASES_H
#define LAUNCHER_CATALOG_ALIASES_H

#include "../core/base.h"

typedef struct CatalogAliasEntry {
    char *key;
    char *friendly_name;
} CatalogAliasEntry;

typedef struct CatalogAliases {
    CatalogAliasEntry *entries;
    u32 count;
} CatalogAliases;

void catalog_aliases_load_json(Arena *arena, const wchar_t *path, CatalogAliases *out);

/* Friendly name when JSON key matches the file leaf name (not used for .lnk shortcut rows). */
const char *catalog_aliases_lookup_filename(const CatalogAliases *aliases, const char *filename_lower_utf8);

#endif
