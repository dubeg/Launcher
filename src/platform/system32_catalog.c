#include "system32_catalog.h"

#include "../core/base.h"

#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

typedef struct AliasEntry {
    char *exe_name;
    char *friendly_name;
} AliasEntry;

typedef struct TempItemList {
    LaunchItem *items;
    u32 count;
    u32 capacity;
} TempItemList;

static void
push_temp_item(TempItemList *list, const LaunchItem *item)
{
    if (list->count >= list->capacity) {
        u32 new_capacity = list->capacity ? list->capacity * 2 : 256;
        list->items = (LaunchItem *)heap_realloc(list->items, sizeof(LaunchItem) * new_capacity);
        list->capacity = new_capacity;
    }
    list->items[list->count++] = *item;
}

static AliasEntry *
parse_alias_entries(Arena *arena, const wchar_t *path, u32 *out_count)
{
    *out_count = 0;
    FileData file = read_entire_file_wide(path);
    if (!file.data) {
        return NULL;
    }

    char *text = (char *)file.data;
    u32 capacity = 32;
    AliasEntry *entries = (AliasEntry *)arena_push_zero(arena, sizeof(AliasEntry) * capacity, sizeof(void *));
    char *cursor = text;
    while ((cursor = strchr(cursor, '"')) != NULL) {
        char *key_start = cursor + 1;
        char *key_end = strchr(key_start, '"');
        if (!key_end) {
            break;
        }
        cursor = strchr(key_end, ':');
        if (!cursor) {
            break;
        }
        cursor = strchr(cursor, '"');
        if (!cursor) {
            break;
        }
        char *value_start = cursor + 1;
        char *value_end = strchr(value_start, '"');
        if (!value_end) {
            break;
        }

        if (*out_count >= capacity) {
            u32 old_capacity = capacity;
            capacity *= 2;
            AliasEntry *new_entries = (AliasEntry *)arena_push_zero(arena, sizeof(AliasEntry) * capacity, sizeof(void *));
            memcpy(new_entries, entries, sizeof(AliasEntry) * old_capacity);
            entries = new_entries;
        }
        entries[*out_count].exe_name = arena_strndup(arena, key_start, (size_t)(key_end - key_start));
        lowercase_ascii_in_place(entries[*out_count].exe_name);
        entries[*out_count].friendly_name = arena_strndup(arena, value_start, (size_t)(value_end - value_start));
        ++(*out_count);
        cursor = value_end + 1;
    }

    free_file_data(&file);
    return entries;
}

static const char *
lookup_alias(const AliasEntry *entries, u32 count, const char *exe_name)
{
    for (u32 i = 0; i < count; ++i) {
        if (_stricmp(entries[i].exe_name, exe_name) == 0) {
            return entries[i].friendly_name;
        }
    }
    return NULL;
}

bool
system32_catalog_build(Arena *arena, const wchar_t *alias_json_path, LaunchItemArray *out_items)
{
    out_items->items = NULL;
    out_items->count = 0;

    wchar_t system_dir[MAX_PATH];
    UINT len = GetSystemDirectoryW(system_dir, array_count(system_dir));
    if (!len) {
        return false;
    }

    u32 alias_count = 0;
    AliasEntry *aliases = parse_alias_entries(arena, alias_json_path, &alias_count);

    wchar_t pattern[MAX_PATH * 2];
    _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\*.exe", system_dir);

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }

    TempItemList temp = {0};
    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        wchar_t full_path[MAX_PATH * 2];
        _snwprintf_s(full_path, array_count(full_path), _TRUNCATE, L"%ls\\%ls", system_dir, find_data.cFileName);

        char exe_name[260];
        utf8_from_wide_buffer(find_data.cFileName, exe_name, array_count(exe_name));
        lowercase_ascii_in_place(exe_name);

        const char *friendly = lookup_alias(aliases, alias_count, exe_name);
        const char *display = friendly ? friendly : exe_name;

        size_t search_size = strlen(display) + 1 + strlen(exe_name) + 1;
        char *search_text = (char *)arena_push_zero(arena, search_size, 1);
        _snprintf_s(search_text, search_size, _TRUNCATE, "%s %s", display, exe_name);
        lowercase_ascii_in_place(search_text);

        char *subtitle = utf8_from_wide(arena, full_path);
        LaunchItem item = {0};
        item.mode = SearchMode_Apps;
        item.source = LaunchSource_System32;
        item.display_name = arena_strdup(arena, display);
        item.search_text = arena_strdup(arena, search_text);
        item.subtitle = arena_strdup(arena, subtitle);
        item.launch_path = arena_wcsdup(arena, full_path);
        push_temp_item(&temp, &item);
    } while (FindNextFileW(find, &find_data));
    FindClose(find);

    out_items->count = temp.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * temp.count, sizeof(void *));
    memcpy(out_items->items, temp.items, sizeof(LaunchItem) * temp.count);
    heap_free(temp.items);
    return true;
}
