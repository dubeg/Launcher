#ifndef LAUNCHER_SYSTEM32_CATALOG_H
#define LAUNCHER_SYSTEM32_CATALOG_H

#include "../app/app.h"

bool system32_catalog_build(Arena *arena, const wchar_t *alias_json_path, LaunchItemArray *out_items);

#endif
