#include "shell_display_name.h"

#include "../core/base.h"

#include <windows.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string.h>

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

static bool
shell_display_wstr_is_useful(const wchar_t *name, const wchar_t *target_path)
{
    if (!name || !name[0] || !target_path || !target_path[0]) {
        return false;
    }
    const wchar_t *leaf = PathFindFileNameW(target_path);
    if (_wcsicmp(name, leaf) == 0) {
        return false;
    }
    wchar_t stem[MAX_PATH];
    if (wcscpy_s(stem, array_count(stem), leaf) != 0) {
        return true;
    }
    PathRemoveExtensionW(stem);
    if (stem[0] && _wcsicmp(name, stem) == 0) {
        return false;
    }
    return true;
}

static bool
try_property_item_name_display(Arena *arena, const wchar_t *parse_path, const wchar_t *target_for_quality, char **out_utf8)
{
    IPropertyStore *store = NULL;
    HRESULT hr = SHGetPropertyStoreFromParsingName(parse_path, NULL, GPS_BESTEFFORT, &IID_IPropertyStore, (void **)&store);
    if (FAILED(hr) || !store) {
        return false;
    }

    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = store->lpVtbl->GetValue(store, &PKEY_ItemNameDisplay, &pv);
    bool ok = false;
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal && pv.pwszVal[0] && shell_display_wstr_is_useful(pv.pwszVal, target_for_quality)) {
        char *utf8 = utf8_from_wide(arena, pv.pwszVal);
        if (utf8 && utf8[0]) {
            *out_utf8 = utf8;
            ok = true;
        }
    }
    PropVariantClear(&pv);
    store->lpVtbl->Release(store);
    return ok;
}

static bool
try_shfileinfo_display_utf8(Arena *arena, const wchar_t *path, const wchar_t *target_for_quality, char **out_utf8)
{
    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));
    if (!SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME)) {
        return false;
    }
    if (!shell_display_wstr_is_useful(sfi.szDisplayName, target_for_quality)) {
        return false;
    }
    char *utf8 = utf8_from_wide(arena, sfi.szDisplayName);
    if (!utf8 || !utf8[0]) {
        return false;
    }
    *out_utf8 = utf8;
    return true;
}

bool
shell_try_item_display_name_utf8(Arena *arena, const wchar_t *shortcut_path_opt, const wchar_t *target_path, char **out_utf8)
{
    if (!arena || !target_path || !target_path[0] || !out_utf8) {
        return false;
    }
    *out_utf8 = NULL;

    if (shortcut_path_opt && shortcut_path_opt[0]) {
        if (try_property_item_name_display(arena, shortcut_path_opt, target_path, out_utf8)) {
            return true;
        }
    }
    if (try_property_item_name_display(arena, target_path, target_path, out_utf8)) {
        return true;
    }
    if (try_shfileinfo_display_utf8(arena, target_path, target_path, out_utf8)) {
        return true;
    }
    return false;
}
