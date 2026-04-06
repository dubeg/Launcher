#include "catalog.h"

#include "start_menu_scan.h"
#include "system32_catalog.h"
#include "../core/base.h"

#include <stdio.h>
#include <string.h>

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
append_directory_as_app(Arena *arena, TempItemList *list, const wchar_t *directory)
{
    wchar_t pattern[MAX_PATH * 4];
    _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\*.exe", directory);

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
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

        LaunchItem item = {0};
        item.mode = SearchMode_Apps;
        item.source = LaunchSource_ExtraPath;
        item.display_name = arena_strdup(arena, exe_name);
        char *dot = strrchr(item.display_name, '.');
        if (dot) {
            *dot = 0;
        }
        item.search_text = arena_strdup(arena, exe_name);
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

static void
parse_locations_json(Arena *arena, TempItemList *list, const wchar_t *path)
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
            append_directory_as_app(arena, list, dir);
        }
        cursor = end + 1;
    }

    free_file_data(&file);
}

bool
app_catalog_build(Arena *arena, const wchar_t *install_dir, LaunchItemArray *out_items)
{
    TempItemList merged = {0};
    LaunchItemArray start_menu = {0};
    LaunchItemArray system32 = {0};

    start_menu_scan_build(arena, &start_menu);

    wchar_t alias_path[MAX_PATH * 4];
    _snwprintf_s(alias_path, array_count(alias_path), _TRUNCATE, L"%ls\\data\\system_aliases.json", install_dir);
    system32_catalog_build(arena, alias_path, &system32);

    append_items(&merged, &start_menu);
    append_items(&merged, &system32);

    wchar_t config_path[MAX_PATH * 4];
    _snwprintf_s(config_path, array_count(config_path), _TRUNCATE, L"%ls\\config\\locations.json", install_dir);
    parse_locations_json(arena, &merged, config_path);

    out_items->count = merged.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * merged.count, sizeof(void *));
    memcpy(out_items->items, merged.items, sizeof(LaunchItem) * merged.count);
    heap_free(merged.items);
    return true;
}
