#include "launch.h"

#include <shellapi.h>

bool
platform_launch_item(const LaunchItem *item)
{
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
