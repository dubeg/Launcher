#ifndef LAUNCHER_SHELL_DISPLAY_NAME_H
#define LAUNCHER_SHELL_DISPLAY_NAME_H

#include "../core/base.h"

bool shell_try_item_display_name_utf8(Arena *arena, const wchar_t *shortcut_path_opt, const wchar_t *target_path, char **out_utf8);

#endif
