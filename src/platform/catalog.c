#include "catalog.h"

#include "catalog_aliases.h"
#include "start_menu_scan.h"
#include "system32_catalog.h"
#include "../core/base.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

/* Cap for recursive extra-path scans (per configured root). */
#define EXTRA_PATH_RECURSIVE_MAX_ITEMS 40000

/* Exe often lives in build\Debug while data\ is at repo root — walk up until relative path exists. */
bool
catalog_resolve_install_relative(const wchar_t *start_dir, const wchar_t *relative_path, wchar_t *out, size_t out_cap_chars)
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

static const wchar_t *const k_extra_path_globs[] = {
    L"*.exe",
    L"*.msc",
    L"*.cpl",
    L"*.com",
    L"*.bat",
    L"*.cmd",
};

static bool
extension_is_extra_program(const wchar_t *ext)
{
    if (!ext || !ext[0]) {
        return false;
    }
    return _wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".msc") == 0 || _wcsicmp(ext, L".cpl") == 0
        || _wcsicmp(ext, L".com") == 0 || _wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0;
}

static void
push_extra_path_item(Arena *arena, TempItemList *list, const wchar_t *full_path, const wchar_t *file_name, const CatalogAliases *aliases)
{
    char file_utf8[520];
    utf8_from_wide_buffer(file_name, file_utf8, array_count(file_utf8));

    char exe_key[260];
    strcpy_s(exe_key, sizeof(exe_key), file_utf8);
    lowercase_ascii_in_place(exe_key);

    char *display_final = NULL;
    const char *alias_name = catalog_aliases_lookup_filename(aliases, exe_key);
    if (alias_name) {
        display_final = arena_strdup(arena, alias_name);
    } else {
        char stem[520];
        strcpy_s(stem, sizeof(stem), file_utf8);
        char *dot = strrchr(stem, '.');
        if (dot) {
            *dot = 0;
        }
        display_final = arena_strdup(arena, stem);
    }

    size_t st_len = strlen(display_final) + 1 + strlen(exe_key) + 1;
    char *search_combined = (char *)arena_push_zero(arena, st_len, 1);
    _snprintf_s(search_combined, st_len, _TRUNCATE, "%s %s", display_final, exe_key);
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
}

static void
append_extra_directory_flat(Arena *arena, TempItemList *list, const wchar_t *directory, const CatalogAliases *aliases)
{
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
            push_extra_path_item(arena, list, full_path, find_data.cFileName, aliases);
        } while (FindNextFileW(find, &find_data));

        FindClose(find);
    }
}

static void
append_extra_directory_recursive(Arena *arena, TempItemList *list, const wchar_t *current_dir, const CatalogAliases *aliases)
{
    if (list->count >= EXTRA_PATH_RECURSIVE_MAX_ITEMS) {
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
            append_extra_directory_recursive(arena, list, child, aliases);
        } else {
            const wchar_t *ext = PathFindExtensionW(find_data.cFileName);
            if (extension_is_extra_program(ext)) {
                push_extra_path_item(arena, list, child, find_data.cFileName, aliases);
            }
        }
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
}

static void
append_extra_location(Arena *arena, TempItemList *list, const wchar_t *directory, const CatalogAliases *aliases, bool recursive)
{
    if (!directory || !directory[0]) {
        return;
    }
    DWORD attr = GetFileAttributesW(directory);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return;
    }
    if (recursive) {
        append_extra_directory_recursive(arena, list, directory, aliases);
    } else {
        append_extra_directory_flat(arena, list, directory, aliases);
    }
}

static void
skip_json_ws(char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') {
        (*p)++;
    }
}

static bool
parse_json_string_content(char **p, Arena *arena, char **out)
{
    skip_json_ws(p);
    if (**p != '"') {
        return false;
    }
    (*p)++;
    char *r = *p;
    size_t n = 0;
    while (*r && *r != '"') {
        if (*r == '\\' && r[1]) {
            n++;
            r += 2;
        } else {
            n++;
            r++;
        }
    }
    if (*r != '"') {
        return false;
    }
    char *buf = (char *)arena_push(arena, n + 1, 1);
    r = *p;
    char *w = buf;
    while (*r != '"') {
        if (*r == '\\' && r[1]) {
            *w++ = r[1];
            r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    *p = r + 1;
    *out = buf;
    return true;
}

static bool
parse_json_bool(char **p, bool *out)
{
    skip_json_ws(p);
    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        *out = true;
        return true;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        *out = false;
        return true;
    }
    return false;
}

static bool
skip_json_value(char **p, Arena *arena)
{
    skip_json_ws(p);
    if (**p == '"') {
        char *dummy;
        return parse_json_string_content(p, arena, &dummy);
    }
    bool b;
    if (parse_json_bool(p, &b)) {
        return true;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return true;
    }
    if (**p == '{') {
        (*p)++;
        for (;;) {
            skip_json_ws(p);
            if (**p == '}') {
                (*p)++;
                return true;
            }
            char *key_tmp = NULL;
            if (!parse_json_string_content(p, arena, &key_tmp)) {
                return false;
            }
            (void)key_tmp;
            skip_json_ws(p);
            if (**p != ':') {
                return false;
            }
            (*p)++;
            if (!skip_json_value(p, arena)) {
                return false;
            }
            skip_json_ws(p);
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == '}') {
                (*p)++;
                return true;
            }
            return false;
        }
    }
    if (**p == '[') {
        (*p)++;
        for (;;) {
            skip_json_ws(p);
            if (**p == ']') {
                (*p)++;
                return true;
            }
            if (!skip_json_value(p, arena)) {
                return false;
            }
            skip_json_ws(p);
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == ']') {
                (*p)++;
                return true;
            }
            return false;
        }
    }
    while (**p && **p != ',' && **p != '}' && **p != ']') {
        (*p)++;
    }
    return true;
}

static bool
is_windows_path_utf8(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    if (s[0] == '\\' && s[1] == '\\') {
        return true;
    }
    unsigned char c0 = (unsigned char)s[0];
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
        if (s[1] == ':' && (s[2] == '\\' || s[2] == '/')) {
            return true;
        }
    }
    return false;
}

static bool
parse_locations_path_object(char **p, Arena *arena, char **out_path, bool *out_recursive)
{
    skip_json_ws(p);
    if (**p != '{') {
        return false;
    }
    (*p)++;
    *out_path = NULL;
    *out_recursive = false;

    for (;;) {
        skip_json_ws(p);
        if (**p == '}') {
            (*p)++;
            break;
        }
        char *key = NULL;
        if (!parse_json_string_content(p, arena, &key)) {
            return false;
        }
        skip_json_ws(p);
        if (**p != ':') {
            return false;
        }
        (*p)++;
        skip_json_ws(p);
        if (strcmp(key, "path") == 0) {
            if (!parse_json_string_content(p, arena, out_path)) {
                return false;
            }
        } else if (strcmp(key, "recursive") == 0) {
            if (!parse_json_bool(p, out_recursive)) {
                return false;
            }
        } else {
            if (!skip_json_value(p, arena)) {
                return false;
            }
        }
        skip_json_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        return false;
    }
    return *out_path != NULL;
}

static char *
loc_json_skip_utf8_bom(char *text)
{
    if (text && (unsigned char)text[0] == 0xefu && (unsigned char)text[1] == 0xbbu && (unsigned char)text[2] == 0xbfu) {
        return text + 3;
    }
    return text;
}

static char *
loc_json_find_paths_array(char *text)
{
    char *p = strstr(text, "\"paths\"");
    if (!p) {
        return NULL;
    }
    p += 7;
    while (*p && *p != '[') {
        p++;
    }
    if (*p != '[') {
        return NULL;
    }
    return p + 1;
}

static void
parse_locations_json(Arena *arena, TempItemList *list, const wchar_t *path, const CatalogAliases *aliases)
{
    FileData file = read_entire_file_wide(path);
    if (!file.data) {
        return;
    }

    char *text = loc_json_skip_utf8_bom((char *)file.data);
    char *p = loc_json_find_paths_array(text);
    if (!p) {
        if (file.size > 0) {
            debug_log_wide(L"locations.json: no \"paths\" array (file=%ls)", path);
            launcher_warning_fmt(
                L"Launcher",
                L"config\\locations.json does not contain a \"paths\" array (or it is not valid JSON at the top level).\n\n"
                L"Extra install folders will be ignored until this is fixed.\n\n%ls",
                path);
        }
        free_file_data(&file);
        return;
    }

    bool malformed = false;
    for (;;) {
        skip_json_ws(&p);
        if (*p == ']') {
            p++;
            break;
        }
        if (*p == 0) {
            malformed = true;
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }

        char *path_utf8 = NULL;
        bool recursive = false;
        if (*p == '"') {
            if (!parse_json_string_content(&p, arena, &path_utf8)) {
                malformed = true;
                break;
            }
        } else if (*p == '{') {
            if (!parse_locations_path_object(&p, arena, &path_utf8, &recursive)) {
                malformed = true;
                break;
            }
        } else {
            malformed = true;
            break;
        }

        if (is_windows_path_utf8(path_utf8)) {
            wchar_t *wdir = wide_from_utf8(arena, path_utf8);
            append_extra_location(arena, list, wdir, aliases, recursive);
        }

        skip_json_ws(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        if (*p == 0) {
            malformed = true;
            break;
        }
        malformed = true;
        break;
    }

    if (malformed) {
        debug_log_wide(L"locations.json: parse error (file=%ls)", path);
        launcher_warning_fmt(
            L"Launcher",
            L"config\\locations.json has a syntax error in the \"paths\" list (for example a stray comma, missing bracket, or bad string).\n\n"
            L"Extra install folders from this file will be ignored until it is fixed.\n\n%ls",
            path);
    }

    free_file_data(&file);
}

static wchar_t *
heap_wcsdup_catalog(const wchar_t *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = (wcslen(s) + 1u) * sizeof(wchar_t);
    wchar_t *p = (wchar_t *)heap_alloc_zero(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static void
catalog_watch_roots_push_unique(CatalogWatchRootDirs *roots, const wchar_t *path)
{
    if (!roots || !path || !path[0]) {
        return;
    }
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return;
    }
    for (u32 i = 0; i < roots->count; ++i) {
        if (roots->paths[i] && _wcsicmp(roots->paths[i], path) == 0) {
            return;
        }
    }
    wchar_t **next = (wchar_t **)heap_realloc(roots->paths, sizeof(wchar_t *) * ((size_t)roots->count + 1u));
    if (!next) {
        return;
    }
    roots->paths = next;
    wchar_t *copy = heap_wcsdup_catalog(path);
    if (!copy) {
        return;
    }
    roots->paths[roots->count++] = copy;
}

static void
watch_append_locations_json_roots(CatalogWatchRootDirs *roots, const wchar_t *json_path)
{
    FileData file = read_entire_file_wide(json_path);
    if (!file.data) {
        return;
    }

    Arena scratch = arena_create(megabytes(1), kilobytes(64));
    char *text = loc_json_skip_utf8_bom((char *)file.data);
    char *p = loc_json_find_paths_array(text);
    if (!p) {
        if (file.size > 0) {
            debug_log_wide(L"locations.json: no \"paths\" array (file=%ls)", json_path);
        }
        arena_destroy(&scratch);
        free_file_data(&file);
        return;
    }

    bool malformed = false;
    for (;;) {
        skip_json_ws(&p);
        if (*p == ']') {
            p++;
            break;
        }
        if (*p == 0) {
            malformed = true;
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }

        char *path_utf8 = NULL;
        bool recursive = false;
        if (*p == '"') {
            if (!parse_json_string_content(&p, &scratch, &path_utf8)) {
                malformed = true;
                break;
            }
        } else if (*p == '{') {
            if (!parse_locations_path_object(&p, &scratch, &path_utf8, &recursive)) {
                malformed = true;
                break;
            }
        } else {
            malformed = true;
            break;
        }

        if (is_windows_path_utf8(path_utf8)) {
            wchar_t *wdir = wide_from_utf8(&scratch, path_utf8);
            catalog_watch_roots_push_unique(roots, wdir);
        }

        skip_json_ws(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        if (*p == 0) {
            malformed = true;
            break;
        }
        malformed = true;
        break;
    }

    if (malformed) {
        debug_log_wide(L"locations.json: parse error (file=%ls)", json_path);
    }

    arena_destroy(&scratch);
    free_file_data(&file);
}

bool
catalog_watch_root_dirs_collect(const wchar_t *install_dir, CatalogWatchRootDirs *out)
{
    if (!out) {
        return false;
    }
    ZeroMemory(out, sizeof(*out));

    PWSTR kf = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &kf))) {
        catalog_watch_roots_push_unique(out, kf);
        CoTaskMemFree(kf);
        kf = NULL;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &kf))) {
        catalog_watch_roots_push_unique(out, kf);
        CoTaskMemFree(kf);
        kf = NULL;
    }

    wchar_t win_dir[MAX_PATH];
    UINT wlen = GetWindowsDirectoryW(win_dir, array_count(win_dir));
    if (wlen > 0 && wlen < array_count(win_dir)) {
        catalog_watch_roots_push_unique(out, win_dir);
    }
    wchar_t sys_dir[MAX_PATH];
    UINT slen = GetSystemDirectoryW(sys_dir, array_count(sys_dir));
    if (slen > 0 && slen < array_count(sys_dir)) {
        catalog_watch_roots_push_unique(out, sys_dir);
    }

    wchar_t config_path[MAX_PATH * 4];
    if (catalog_resolve_install_relative(install_dir, L"config\\locations.json", config_path, array_count(config_path))) {
        watch_append_locations_json_roots(out, config_path);
    }

    return true;
}

void
catalog_watch_root_dirs_free(CatalogWatchRootDirs *dirs)
{
    if (!dirs) {
        return;
    }
    for (u32 i = 0; i < dirs->count; ++i) {
        heap_free(dirs->paths[i]);
    }
    heap_free(dirs->paths);
    dirs->paths = NULL;
    dirs->count = 0;
}

bool
app_catalog_build(Arena *arena, const wchar_t *install_dir, LaunchItemArray *out_items, CatalogAliases *out_aliases)
{
    TempItemList merged = {0};
    LaunchItemArray start_menu = {0};
    LaunchItemArray system32 = {0};

    wchar_t alias_path[MAX_PATH * 4];
    if (!catalog_resolve_install_relative(install_dir, L"data\\system_aliases.json", alias_path, array_count(alias_path))) {
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
    if (catalog_resolve_install_relative(install_dir, L"config\\locations.json", config_path, array_count(config_path))) {
        parse_locations_json(arena, &merged, config_path, &aliases);
    }

    out_items->count = merged.count;
    out_items->items = (LaunchItem *)arena_push(arena, sizeof(LaunchItem) * merged.count, sizeof(void *));
    memcpy(out_items->items, merged.items, sizeof(LaunchItem) * merged.count);
    heap_free(merged.items);
    return true;
}
