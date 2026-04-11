#ifndef LAUNCHER_EVERYTHING_CLIENT_H
#define LAUNCHER_EVERYTHING_CLIENT_H

#include "../app/app.h"
#include "catalog_aliases.h"

typedef struct EverythingQueryResult {
    bool available;
    LaunchItemArray items;
} EverythingQueryResult;

EverythingQueryResult everything_query_files(Arena *arena, const char *query_utf8, u32 max_results, const CatalogAliases *aliases);

/* True if query uses Everything glob metacharacters (* or ?). */
bool everything_query_has_glob_wildcards(const char *query_utf8);

#endif
