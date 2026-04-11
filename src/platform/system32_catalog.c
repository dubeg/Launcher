#include "system32_catalog.h"
#include "catalog_aliases.h"
#include "shell_display_name.h"

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
path_contains_ci(const wchar_t *path, const wchar_t *needle)
{
    if (!path || !needle || !needle[0]) {
        return false;
    }
    size_t n = wcslen(needle);
    for (const wchar_t *p = path; *p; ++p) {
        if (_wcsnicmp(p, needle, (int)n) == 0) {
            return true;
        }
    }
    return false;
}

static bool
windows_scan_should_skip_dir(const wchar_t *full_path)
{
    static const wchar_t *const k_skip_markers[] = {
        L"\\WinSxS\\",
        L"\\servicing\\",
        L"\\assembly\\",
        L"\\Installer\\",
        L"\\SoftwareDistribution\\",
        L"\\System32\\DriverStore\\",
        L"\\SysWOW64\\DriverStore\\",
        L"\\WinRE\\",
        L"\\ModemLogs\\",
        L"\\Logs\\CBS\\",
    };
    for (u32 i = 0; i < array_count(k_skip_markers); ++i) {
        if (path_contains_ci(full_path, k_skip_markers[i])) {
            return true;
        }
    }
    return false;
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

    char exe_name[260];
    utf8_from_wide_buffer(file_name, exe_name, array_count(exe_name));
    lowercase_ascii_in_place(exe_name);

    char *shell_display = NULL;
    const char *friendly = catalog_aliases_lookup_filename(aliases, exe_name);
    const char *display = NULL;
    if (friendly) {
        display = friendly;
    } else if (shell_try_item_display_name_utf8(arena, NULL, full_path, &shell_display) && shell_display) {
        display = shell_display;
    } else {
        display = exe_name;
    }

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
    push_temp_item(list, &item);
}

static void
scan_windows_dir_recursive(Arena *arena, TempItemList *list, const CatalogAliases *aliases, const wchar_t *current_dir)
{
    if (list->count >= WINDOWS_CATALOG_MAX_ITEMS) {
        return;
    }
    if (windows_scan_should_skip_dir(current_dir)) {
        return;
    }

    wchar_t pattern[MAX_PATH * 4];
    _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\*", current_dir);

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

        wchar_t child[MAX_PATH * 4];
        _snwprintf_s(child, array_count(child), _TRUNCATE, L"%ls\\%ls", current_dir, find_data.cFileName);

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }
            scan_windows_dir_recursive(arena, list, aliases, child);
        } else {
            const wchar_t *ext = PathFindExtensionW(find_data.cFileName);
            if (extension_is_windows_program(ext)) {
                append_windows_program_file(arena, list, aliases, child, find_data.cFileName);
            }
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
    scan_windows_dir_recursive(arena, &temp, aliases, win_dir);

    if (temp.count == 0) {
        return false;
    }

    out_items->count = temp.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * temp.count, sizeof(void *));
    memcpy(out_items->items, temp.items, sizeof(LaunchItem) * temp.count);
    heap_free(temp.items);
    return true;
}
