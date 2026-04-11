#ifndef LAUNCHER_LAUNCH_H
#define LAUNCHER_LAUNCH_H

#include "../app/app.h"

bool platform_launch_item(const LaunchItem *item);
bool platform_open_file_location(const LaunchItem *item);

#endif
