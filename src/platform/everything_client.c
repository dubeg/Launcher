#include "everything_client.h"
#include "catalog_aliases.h"

#include "../core/base.h"

#define EVERYTHINGUSERAPI
#include "../../third_party/everything_sdk/sdk/include/Everything.h"

static wchar_t *
build_query_wide(Arena *arena, const char *query_utf8)
{
    wchar_t *wide = wide_from_utf8(arena, query_utf8);
    return wide;
}

EverythingQueryResult
everything_query_files(Arena *arena, const char *query_utf8, u32 max_results, const CatalogAliases *aliases)
{
    EverythingQueryResult result = {0};
    if (!query_utf8 || !query_utf8[0]) {
        result.available = true;
        return result;
    }

    wchar_t *wide_query = build_query_wide(arena, query_utf8);
    Everything_Reset();
    Everything_SetSearchW(wide_query);
    Everything_SetMatchCase(FALSE);
    Everything_SetMatchPath(FALSE);
    Everything_SetRegex(FALSE);
    Everything_SetMax(max_results);
    Everything_SetSort(EVERYTHING_SORT_NAME_ASCENDING);
    Everything_SetRequestFlags(EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME);

    if (!Everything_QueryW(TRUE)) {
        DWORD error = Everything_GetLastError();
        result.available = (error != EVERYTHING_ERROR_IPC);
        return result;
    }

    result.available = true;
    DWORD count = Everything_GetNumResults();
    result.items = (LaunchItemArray){0};
    result.items.count = count;
    result.items.items = (LaunchItem *)arena_push_zero(arena, sizeof(LaunchItem) * count, sizeof(void *));

    for (DWORD i = 0; i < count; ++i) {
        wchar_t full_path[MAX_PATH * 8];
        Everything_GetResultFullPathNameW(i, full_path, array_count(full_path));

        char *full_path_utf8 = utf8_from_wide(arena, full_path);
        char *file_name = path_filename_utf8(full_path_utf8);
        char file_key[260];
        strcpy_s(file_key, sizeof(file_key), file_name);
        lowercase_ascii_in_place(file_key);
        const char *alias_title = catalog_aliases_lookup_msc_cpl(aliases, file_key);
        char *search_text = NULL;
        if (alias_title) {
            size_t st_len = strlen(full_path_utf8) + 1 + strlen(alias_title) + 1 + strlen(file_name) + 1;
            search_text = (char *)arena_push_zero(arena, st_len, 1);
            _snprintf_s(search_text, st_len, _TRUNCATE, "%s %s %s", full_path_utf8, alias_title, file_name);
            lowercase_ascii_in_place(search_text);
        } else {
            search_text = arena_strdup(arena, full_path_utf8);
            lowercase_ascii_in_place(search_text);
        }

        LaunchItem *item = &result.items.items[i];
        item->mode = SearchMode_Files;
        item->source = LaunchSource_Everything;
        item->display_name = arena_strdup(arena, alias_title ? alias_title : file_name);
        item->search_text = arena_strdup(arena, search_text);
        item->subtitle = arena_strdup(arena, full_path_utf8);
        item->launch_path = arena_wcsdup(arena, full_path);
    }

    return result;
}
