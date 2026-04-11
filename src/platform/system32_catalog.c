#include "system32_catalog.h"
#include "catalog_aliases.h"
#include "shell_display_name.h"

#include "../core/base.h"

#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

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

bool
system32_catalog_build(Arena *arena, const CatalogAliases *aliases, LaunchItemArray *out_items)
{
    out_items->items = NULL;
    out_items->count = 0;

    wchar_t system_dir[MAX_PATH];
    UINT len = GetSystemDirectoryW(system_dir, array_count(system_dir));
    if (!len) {
        return false;
    }

    static const wchar_t *const k_system32_globs[] = {
        L"*.exe",
        L"*.msc",
        L"*.cpl",
        L"*.com",
        L"*.bat",
        L"*.cmd",
    };

    TempItemList temp = {0};
    for (u32 g = 0; g < array_count(k_system32_globs); ++g) {
        wchar_t pattern[MAX_PATH * 2];
        _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\%ls", system_dir, k_system32_globs[g]);

        WIN32_FIND_DATAW find_data;
        HANDLE find = FindFirstFileW(pattern, &find_data);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            wchar_t full_path[MAX_PATH * 2];
            _snwprintf_s(full_path, array_count(full_path), _TRUNCATE, L"%ls\\%ls", system_dir, find_data.cFileName);

            char exe_name[260];
            utf8_from_wide_buffer(find_data.cFileName, exe_name, array_count(exe_name));
            lowercase_ascii_in_place(exe_name);

            char *shell_display = NULL;
            const char *friendly = catalog_aliases_lookup_msc_cpl(aliases, exe_name);
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
            push_temp_item(&temp, &item);
        } while (FindNextFileW(find, &find_data));
        FindClose(find);
    }

    if (temp.count == 0) {
        return false;
    }

    out_items->count = temp.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * temp.count, sizeof(void *));
    memcpy(out_items->items, temp.items, sizeof(LaunchItem) * temp.count);
    heap_free(temp.items);
    return true;
}
