#ifndef LAUNCHER_APP_H
#define LAUNCHER_APP_H

#include "../core/base.h"

#define LAUNCHER_MAX_QUERY 260
#define LAUNCHER_MAX_RESULTS 48
#define LAUNCHER_HOTKEY_ID 1
#define LAUNCHER_WINDOW_CLASS L"LauncherWindowClass"
#define LAUNCHER_WINDOW_TITLE L"Launcher"

typedef enum SearchMode {
    SearchMode_Apps = 0,
    SearchMode_Files = 1,
} SearchMode;

typedef enum LaunchSource {
    LaunchSource_StartMenu = 0,
    LaunchSource_System32 = 1,
    LaunchSource_ExtraPath = 2,
    LaunchSource_Everything = 3,
} LaunchSource;

typedef struct LaunchItem {
    SearchMode mode;
    LaunchSource source;
    char *display_name;
    char *search_text;
    char *subtitle;
    wchar_t *launch_path;
    wchar_t *arguments;
} LaunchItem;

typedef struct LaunchItemArray {
    LaunchItem *items;
    u32 count;
} LaunchItemArray;

typedef struct SearchResult {
    const LaunchItem *item;
    double score;
} SearchResult;

typedef struct SearchResultArray {
    SearchResult *items;
    u32 count;
} SearchResultArray;

#endif
