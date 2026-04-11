#include "start_menu_scan.h"
#include "catalog_aliases.h"
#include "shell_display_name.h"

#include "../core/base.h"

#include <shlobj.h>
#include <shobjidl.h>
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
        u32 new_capacity = list->capacity ? list->capacity * 2 : 128;
        list->items = (LaunchItem *)heap_realloc(list->items, sizeof(LaunchItem) * new_capacity);
        list->capacity = new_capacity;
    }
    list->items[list->count++] = *item;
}

static bool
resolve_shortcut(const wchar_t *shortcut_path, wchar_t *target, size_t target_count, wchar_t *args, size_t args_count)
{
    IShellLinkW *shell_link = NULL;
    IPersistFile *persist = NULL;
    bool ok = false;

    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&shell_link))) {
        return false;
    }
    if (FAILED(IShellLinkW_QueryInterface(shell_link, &IID_IPersistFile, (void **)&persist))) {
        IShellLinkW_Release(shell_link);
        return false;
    }
    if (SUCCEEDED(IPersistFile_Load(persist, shortcut_path, STGM_READ)) &&
        SUCCEEDED(IShellLinkW_GetPath(shell_link, target, (int)target_count, NULL, SLGP_RAWPATH))) {
        IShellLinkW_GetArguments(shell_link, args, (int)args_count);
        ok = target[0] != 0;
    }

    IPersistFile_Release(persist);
    IShellLinkW_Release(shell_link);
    return ok;
}

static bool
extension_is_program_file(const wchar_t *ext)
{
    if (!ext || !ext[0]) {
        return false;
    }
    return _wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".msc") == 0 || _wcsicmp(ext, L".cpl") == 0
        || _wcsicmp(ext, L".com") == 0 || _wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0
        || _wcsicmp(ext, L".ps1") == 0;
}

static bool
shortcut_link_target_allowed(const wchar_t *target_path)
{
    if (!target_path || !target_path[0]) {
        return false;
    }
    const wchar_t *ext = PathFindExtensionW(target_path);
    if (!ext || !ext[0]) {
        return false;
    }
    return _wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0
        || _wcsicmp(ext, L".ps1") == 0 || _wcsicmp(ext, L".msc") == 0 || _wcsicmp(ext, L".cpl") == 0
        || _wcsicmp(ext, L".com") == 0 || _wcsicmp(ext, L".msi") == 0;
}

static void
append_direct_program_file(Arena *arena, TempItemList *list, const wchar_t *file_path, const CatalogAliases *aliases)
{
    char *path_utf8 = utf8_from_wide(arena, file_path);
    char *display_name_utf8 = wide_path_filename_utf8(arena, file_path);
    char *search_base = arena_strdup(arena, display_name_utf8);
    char *dot = strrchr(search_base, '.');
    if (dot) {
        *dot = 0;
    }

    char exe_key[260];
    strcpy_s(exe_key, sizeof(exe_key), path_filename_utf8(path_utf8));
    lowercase_ascii_in_place(exe_key);

    char *shell_display = NULL;
    char *display_final = NULL;
    const char *alias_name = catalog_aliases_lookup_msc_cpl(aliases, exe_key);
    if (alias_name) {
        display_final = arena_strdup(arena, alias_name);
    } else if (shell_try_item_display_name_utf8(arena, NULL, file_path, &shell_display) && shell_display) {
        display_final = arena_strdup(arena, shell_display);
    } else {
        display_final = arena_strdup(arena, search_base);
    }

    size_t search_size = strlen(display_final) + 1 + strlen(exe_key) + 1;
    char *combined = (char *)arena_push_zero(arena, search_size, 1);
    _snprintf_s(combined, search_size, _TRUNCATE, "%s %s", display_final, exe_key);
    lowercase_ascii_in_place(combined);

    LaunchItem item = {0};
    item.mode = SearchMode_Apps;
    item.source = LaunchSource_StartMenu;
    item.display_name = display_final;
    item.search_text = arena_strdup(arena, combined);
    item.subtitle = arena_strdup(arena, path_utf8);
    item.launch_path = arena_wcsdup(arena, file_path);
    item.arguments = NULL;
    push_temp_item(list, &item);
}

static void
append_shortcut(Arena *arena, TempItemList *list, const wchar_t *shortcut_path)
{
    wchar_t target[MAX_PATH * 4] = {0};
    wchar_t arguments[512] = {0};
    if (!resolve_shortcut(shortcut_path, target, array_count(target), arguments, array_count(arguments))) {
        return;
    }
    if (!shortcut_link_target_allowed(target)) {
        return;
    }

    char *target_utf8 = utf8_from_wide(arena, target);
    char exe_key[260];
    strcpy_s(exe_key, sizeof(exe_key), path_filename_utf8(target_utf8));
    lowercase_ascii_in_place(exe_key);

    char *shell_display = NULL;
    char *display_final = NULL;
    if (shell_try_item_display_name_utf8(arena, shortcut_path, target, &shell_display) && shell_display) {
        display_final = arena_strdup(arena, shell_display);
    } else {
        char *display_name_utf8 = wide_path_filename_utf8(arena, shortcut_path);
        display_final = arena_strdup(arena, display_name_utf8);
        char *dot = strrchr(display_final, '.');
        if (dot) {
            *dot = 0;
        }
    }

    size_t search_size = strlen(display_final) + 1 + strlen(exe_key) + 1;
    char *combined = (char *)arena_push_zero(arena, search_size, 1);
    _snprintf_s(combined, search_size, _TRUNCATE, "%s %s", display_final, exe_key);
    lowercase_ascii_in_place(combined);

    LaunchItem item = {0};
    item.mode = SearchMode_Apps;
    item.source = LaunchSource_StartMenu;
    item.display_name = display_final;
    item.search_text = arena_strdup(arena, combined);
    item.subtitle = arena_strdup(arena, target_utf8);
    item.launch_path = arena_wcsdup(arena, target);
    item.arguments = arguments[0] ? arena_wcsdup(arena, arguments) : NULL;
    push_temp_item(list, &item);
}

static void
scan_directory_recursive(Arena *arena, TempItemList *list, const wchar_t *directory, const CatalogAliases *aliases)
{
    wchar_t pattern[MAX_PATH * 4];
    _snwprintf_s(pattern, array_count(pattern), _TRUNCATE, L"%ls\\*", directory);

    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (wcscmp(find_data.cFileName, L".") == 0 || wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }

        wchar_t full_path[MAX_PATH * 4];
        _snwprintf_s(full_path, array_count(full_path), _TRUNCATE, L"%ls\\%ls", directory, find_data.cFileName);

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_directory_recursive(arena, list, full_path, aliases);
        } else {
            const wchar_t *ext = PathFindExtensionW(find_data.cFileName);
            if (_wcsicmp(ext, L".lnk") == 0) {
                append_shortcut(arena, list, full_path);
            } else if (extension_is_program_file(ext)) {
                append_direct_program_file(arena, list, full_path, aliases);
            }
        }
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
}

static void
scan_known_folder(Arena *arena, TempItemList *list, const GUID *folder_id, const CatalogAliases *aliases)
{
    PWSTR path = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(folder_id, 0, NULL, &path))) {
        scan_directory_recursive(arena, list, path, aliases);
        CoTaskMemFree(path);
    }
}

bool
start_menu_scan_build(Arena *arena, const CatalogAliases *aliases, LaunchItemArray *out_items)
{
    TempItemList temp = {0};
    scan_known_folder(arena, &temp, &FOLDERID_Programs, aliases);
    scan_known_folder(arena, &temp, &FOLDERID_CommonPrograms, aliases);

    out_items->count = temp.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * temp.count, sizeof(void *));
    memcpy(out_items->items, temp.items, sizeof(LaunchItem) * temp.count);
    heap_free(temp.items);
    return true;
}
