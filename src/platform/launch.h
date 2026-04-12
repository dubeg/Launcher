#ifndef LAUNCHER_LAUNCH_H
#define LAUNCHER_LAUNCH_H

#include <windows.h>

#include "../app/app.h"

bool platform_launch_item(const LaunchItem *item, bool elevated);
bool platform_open_file_location(const LaunchItem *item);
bool platform_show_file_properties(HWND owner, const wchar_t *path);

#endif
