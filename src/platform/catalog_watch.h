#ifndef LAUNCHER_CATALOG_WATCH_H
#define LAUNCHER_CATALOG_WATCH_H

#include <windows.h>

/* Posted when a watched install / Start Menu / locations root changes (debounce in the window procedure). */
#define WM_LAUNCHER_CATALOG_FS_CHANGED (WM_APP + 120)

void catalog_watch_start(HWND hwnd, const wchar_t *install_dir);
void catalog_watch_stop(void);

#endif
