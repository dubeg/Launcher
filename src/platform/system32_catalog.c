#include "system32_catalog.h"
#include "catalog_aliases.h"

#include "../core/base.h"

#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

/* Stop runaway trees (WinSxS-style duplication, accidental mounts). */
#define WINDOWS_CATALOG_MAX_ITEMS 60000

typedef struct TempItemList {
    LaunchItem *items;
    u32 count;
    u32 capacity;
} TempItemList;

static void
push_temp_item(TempItemList *list, const LaunchItem *item)
{
    if (list->count >= WINDOWS_CATALOG_MAX_ITEMS) {
        return;
    }
    if (list->count >= list->capacity) {
        u32 new_capacity = list->capacity ? list->capacity * 2 : 256;
        list->items = (LaunchItem *)heap_realloc(list->items, sizeof(LaunchItem) * new_capacity);
        list->capacity = new_capacity;
    }
    list->items[list->count++] = *item;
}

static bool
extension_is_windows_program(const wchar_t *ext)
{
    if (!ext || !ext[0]) {
        return false;
    }
    return _wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".msc") == 0 || _wcsicmp(ext, L".cpl") == 0;
}

static void
append_windows_program_file(Arena *arena, TempItemList *list, const CatalogAliases *aliases, const wchar_t *full_path,
                             const wchar_t *file_name)
{
    if (list->count >= WINDOWS_CATALOG_MAX_ITEMS) {
        return;
    }

    char file_utf8[520];
    utf8_from_wide_buffer(file_name, file_utf8, array_count(file_utf8));

    char exe_key[260];
    strcpy_s(exe_key, sizeof(exe_key), file_utf8);
    lowercase_ascii_in_place(exe_key);

    char stem[520];
    strcpy_s(stem, sizeof(stem), file_utf8);
    char *dot = strrchr(stem, '.');
    if (dot) {
        *dot = 0;
    }

    const char *friendly = catalog_aliases_lookup_filename(aliases, exe_key);
    const char *display = NULL;
    if (friendly) {
        display = friendly;
    } else {
        display = stem;
    }

    size_t search_size = strlen(display) + 1 + strlen(exe_key) + 1;
    char *search_text = (char *)arena_push_zero(arena, search_size, 1);
    _snprintf_s(search_text, search_size, _TRUNCATE, "%s %s", display, exe_key);
    lowercase_ascii_in_place(search_text);

    char *subtitle = utf8_from_wide(arena, full_path);
    LaunchItem item = {0};
    item.mode = SearchMode_Apps;
    item.source = LaunchSource_System32;
    item.display_name = arena_strdup(arena, display);
    item.search_text = arena_strdup(arena, search_text);
    item.subtitle = arena_strdup(arena, subtitle);
    item.launch_path = arena_wcsdup(arena, full_path);
    push_temp_item(list, &item);
}

/*
 * Only direct children of `dir` (no subfolders). Avoids locale trees like System32\en-US
 * and never visits SysWOW64 when combined with roots below.
 */
static void
scan_windows_dir_flat(Arena *arena, TempItemList *list, const CatalogAliases *aliases, const wchar_t *dir)
{
    if (list->count >= WINDOWS_CATALOG_MAX_ITEMS || !dir || !dir[0]) {
        return;
    }

    wchar_t pattern[MAX_PATH * 4];
    _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\*", dir);

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (find_data.cFileName[0] == L'.' &&
            (find_data.cFileName[1] == 0 || (find_data.cFileName[1] == L'.' && find_data.cFileName[2] == 0))) {
            continue;
        }
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        wchar_t full[MAX_PATH * 4];
        _snwprintf_s(full, array_count(full), _TRUNCATE, L"%ls\\%ls", dir, find_data.cFileName);
        const wchar_t *ext = PathFindExtensionW(find_data.cFileName);
        if (extension_is_windows_program(ext)) {
            append_windows_program_file(arena, list, aliases, full, find_data.cFileName);
        }
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
}

bool
system32_catalog_build(Arena *arena, const CatalogAliases *aliases, LaunchItemArray *out_items)
{
    out_items->items = NULL;
    out_items->count = 0;

    wchar_t win_dir[MAX_PATH];
    UINT len = GetWindowsDirectoryW(win_dir, array_count(win_dir));
    if (!len) {
        return false;
    }

    TempItemList temp = {0};
    scan_windows_dir_flat(arena, &temp, aliases, win_dir);

    wchar_t system32[MAX_PATH * 4];
    _snwprintf_s(system32, array_count(system32), _TRUNCATE, L"%ls\\System32", win_dir);
    scan_windows_dir_flat(arena, &temp, aliases, system32);

    if (temp.count == 0) {
        return false;
    }

    out_items->count = temp.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * temp.count, sizeof(void *));
    memcpy(out_items->items, temp.items, sizeof(LaunchItem) * temp.count);
    heap_free(temp.items);
    return true;
}
