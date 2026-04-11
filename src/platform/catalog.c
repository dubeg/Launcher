#include "catalog.h"

#include "catalog_aliases.h"
#include "shell_display_name.h"
#include "start_menu_scan.h"
#include "system32_catalog.h"
#include "../core/base.h"

#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

/* Exe often lives in build\Debug while data\ is at repo root — walk up until relative path exists. */
static bool
catalog_resolve_existing_file(const wchar_t *start_dir, const wchar_t *relative_path, wchar_t *out, size_t out_cap_chars)
{
    wchar_t base[MAX_PATH * 4];
    if (wcscpy_s(base, array_count(base), start_dir) != 0) {
        return false;
    }
    for (u32 step = 0; step < 24; ++step) {
        _snwprintf_s(out, out_cap_chars, _TRUNCATE, L"%ls\\%ls", base, relative_path);
        DWORD attr = GetFileAttributesW(out);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return true;
        }
        if (!PathRemoveFileSpecW(base)) {
            break;
        }
        if (!base[0]) {
            break;
        }
    }
    return false;
}

typedef struct TempItemList {
    LaunchItem *items;
    u32 count;
    u32 capacity;
} TempItemList;

static void
append_items(TempItemList *list, const LaunchItemArray *array)
{
    if (!array->count) {
        return;
    }
    u32 required = list->count + array->count;
    if (required > list->capacity) {
        u32 new_capacity = list->capacity ? list->capacity : 256;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        list->items = (LaunchItem *)heap_realloc(list->items, sizeof(LaunchItem) * new_capacity);
        list->capacity = new_capacity;
    }
    memcpy(list->items + list->count, array->items, sizeof(LaunchItem) * array->count);
    list->count += array->count;
}

static void
append_directory_as_app(Arena *arena, TempItemList *list, const wchar_t *directory, const CatalogAliases *aliases)
{
    static const wchar_t *const k_extra_path_globs[] = {
        L"*.exe",
        L"*.msc",
        L"*.cpl",
        L"*.com",
        L"*.bat",
        L"*.cmd",
    };

    for (u32 g = 0; g < array_count(k_extra_path_globs); ++g) {
        wchar_t pattern[MAX_PATH * 4];
        _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\%ls", directory, k_extra_path_globs[g]);

        WIN32_FIND_DATAW find_data;
        HANDLE find = FindFirstFileW(pattern, &find_data);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            wchar_t full_path[MAX_PATH * 4];
            _snwprintf_s(full_path, array_count(full_path), _TRUNCATE, L"%ls\\%ls", directory, find_data.cFileName);

            char exe_name[260];
            utf8_from_wide_buffer(find_data.cFileName, exe_name, array_count(exe_name));
            lowercase_ascii_in_place(exe_name);

            char *shell_display = NULL;
            char *display_final = NULL;
            const char *alias_name = catalog_aliases_lookup_msc_cpl(aliases, exe_name);
            if (alias_name) {
                display_final = arena_strdup(arena, alias_name);
            } else if (shell_try_item_display_name_utf8(arena, NULL, full_path, &shell_display) && shell_display) {
                display_final = arena_strdup(arena, shell_display);
            } else {
                char stem[260];
                strcpy_s(stem, sizeof(stem), exe_name);
                char *dot = strrchr(stem, '.');
                if (dot) {
                    *dot = 0;
                }
                display_final = arena_strdup(arena, stem);
            }

            size_t st_len = strlen(display_final) + 1 + strlen(exe_name) + 1;
            char *search_combined = (char *)arena_push_zero(arena, st_len, 1);
            _snprintf_s(search_combined, st_len, _TRUNCATE, "%s %s", display_final, exe_name);
            lowercase_ascii_in_place(search_combined);

            LaunchItem item = {0};
            item.mode = SearchMode_Apps;
            item.source = LaunchSource_ExtraPath;
            item.display_name = display_final;
            item.search_text = arena_strdup(arena, search_combined);
            item.subtitle = utf8_from_wide(arena, full_path);
            item.launch_path = arena_wcsdup(arena, full_path);

            if (list->count >= list->capacity) {
                u32 new_capacity = list->capacity ? list->capacity * 2 : 64;
                list->items = (LaunchItem *)heap_realloc(list->items, sizeof(LaunchItem) * new_capacity);
                list->capacity = new_capacity;
            }
            list->items[list->count++] = item;
        } while (FindNextFileW(find, &find_data));

        FindClose(find);
    }
}

static void
parse_locations_json(Arena *arena, TempItemList *list, const wchar_t *path, const CatalogAliases *aliases)
{
    FileData file = read_entire_file_wide(path);
    if (!file.data) {
        return;
    }

    char *cursor = (char *)file.data;
    while ((cursor = strchr(cursor, '"')) != NULL) {
        char *start = cursor + 1;
        char *end = strchr(start, '"');
        if (!end) {
            break;
        }
        size_t size = (size_t)(end - start);
        if (size > 2 && ((size >= 3 && strncmp(start, "C:\\", 3) == 0) || strncmp(start, "\\\\", 2) == 0)) {
            char *path_utf8 = arena_strndup(arena, start, size);
            wchar_t *dir = wide_from_utf8(arena, path_utf8);
            append_directory_as_app(arena, list, dir, aliases);
        }
        cursor = end + 1;
    }

    free_file_data(&file);
}

bool
app_catalog_build(Arena *arena, const wchar_t *install_dir, LaunchItemArray *out_items, CatalogAliases *out_aliases)
{
    TempItemList merged = {0};
    LaunchItemArray start_menu = {0};
    LaunchItemArray system32 = {0};

    wchar_t alias_path[MAX_PATH * 4];
    if (!catalog_resolve_existing_file(install_dir, L"data\\system_aliases.json", alias_path, array_count(alias_path))) {
        alias_path[0] = 0;
    }
    CatalogAliases aliases = {0};
    if (alias_path[0]) {
        catalog_aliases_load_json(arena, alias_path, &aliases);
    }
    if (out_aliases) {
        *out_aliases = aliases;
    }

    start_menu_scan_build(arena, &aliases, &start_menu);
    system32_catalog_build(arena, &aliases, &system32);

    append_items(&merged, &start_menu);
    append_items(&merged, &system32);

    wchar_t config_path[MAX_PATH * 4];
    if (catalog_resolve_existing_file(install_dir, L"config\\locations.json", config_path, array_count(config_path))) {
        parse_locations_json(arena, &merged, config_path, &aliases);
    }

    out_items->count = merged.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * merged.count, sizeof(void *));
    memcpy(out_items->items, merged.items, sizeof(LaunchItem) * merged.count);
    heap_free(merged.items);
    return true;
}
