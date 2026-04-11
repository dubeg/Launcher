#include "launch.h"

#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static void
path_strip_trailing_separators(wchar_t *path)
{
    size_t n = wcslen(path);
    while (n > 0 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) {
        if (n == 3 && path[1] == L':')
            break;
        path[--n] = L'\0';
    }
}

static bool
path_is_directory_wide(const wchar_t *path)
{
    if (!path || !path[0])
        return false;
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool
launch_path_extension_iequals(const wchar_t *path, const wchar_t *ext_with_dot)
{
    if (!path || !ext_with_dot) {
        return false;
    }
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot) {
        return false;
    }
    return _wcsicmp(dot, ext_with_dot) == 0;
}

bool
platform_launch_item(const LaunchItem *item)
{
    if (item->mode == SearchMode_Files && item->source == LaunchSource_Everything
        && path_is_directory_wide(item->launch_path)) {
        wchar_t windir[MAX_PATH];
        if (GetEnvironmentVariableW(L"WINDIR", windir, array_count(windir)) == 0)
            return false;

        wchar_t explorer_path[MAX_PATH];
        _snwprintf_s(explorer_path, array_count(explorer_path), _TRUNCATE, L"%ls\\explorer.exe", windir);

        wchar_t path_buf[MAX_PATH * 8];
        wcsncpy_s(path_buf, array_count(path_buf), item->launch_path, _TRUNCATE);
        path_strip_trailing_separators(path_buf);

        wchar_t params[MAX_PATH * 8 + 32];
        _snwprintf_s(params, array_count(params), _TRUNCATE, L"/select,\"%ls\"", path_buf);

        SHELLEXECUTEINFOW info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC;
        info.nShow = SW_SHOWNORMAL;
        info.lpVerb = L"open";
        info.lpFile = explorer_path;
        info.lpParameters = params;
        return ShellExecuteExW(&info) == TRUE;
    }

    if (item->launch_path && launch_path_extension_iequals(item->launch_path, L".cpl") && item->arguments == NULL) {
        wchar_t system_dir[MAX_PATH];
        UINT sys_len = GetSystemDirectoryW(system_dir, array_count(system_dir));
        if (sys_len == 0) {
            return false;
        }
        wchar_t control_exe[MAX_PATH * 2];
        _snwprintf_s(control_exe, array_count(control_exe), _TRUNCATE, L"%ls\\control.exe", system_dir);

        wchar_t params[MAX_PATH * 4];
        _snwprintf_s(params, array_count(params), _TRUNCATE, L"\"%ls\"", item->launch_path);

        SHELLEXECUTEINFOW info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC;
        info.nShow = SW_SHOWNORMAL;
        info.lpVerb = NULL;
        info.lpFile = control_exe;
        info.lpParameters = params;
        info.lpDirectory = system_dir;
        return ShellExecuteExW(&info) == TRUE;
    }

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.nShow = SW_SHOWNORMAL;
    info.lpVerb = L"open";
    info.lpFile = item->launch_path;
    info.lpParameters = item->arguments;
    return ShellExecuteExW(&info) == TRUE;
}
