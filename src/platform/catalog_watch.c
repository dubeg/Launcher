#include "catalog_watch.h"

#include "catalog.h"

#include "../core/base.h"

#define CATALOG_WATCH_NOTIFY_BUF (16384u)
/* Leave room for g_stop_event alongside directory notifications (WaitForMultipleObjects limit). */
#define CATALOG_WATCH_MAX_DIRS (MAXIMUM_WAIT_OBJECTS - 2)

typedef struct WatchSlot {
    HANDLE dir;
    OVERLAPPED ov;
    BYTE buffer[CATALOG_WATCH_NOTIFY_BUF];
} WatchSlot;

static HANDLE g_thread;
static HANDLE g_stop_event;
static HWND g_notify_hwnd;
static WatchSlot *g_slots;
static u32 g_slot_count;
static HANDLE *g_wait_handles;

static DWORD WINAPI
catalog_watch_thread_main(void *unused)
{
    (void)unused;
    if (!g_stop_event || !g_notify_hwnd || !g_slots || g_slot_count == 0 || !g_wait_handles) {
        return 0;
    }

    g_wait_handles[0] = g_stop_event;
    for (u32 i = 0; i < g_slot_count; ++i) {
        g_wait_handles[i + 1] = g_slots[i].ov.hEvent;
    }

    for (;;) {
        DWORD w = WaitForMultipleObjects((DWORD)g_slot_count + 1u, g_wait_handles, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (w < WAIT_OBJECT_0 + 1u || w > WAIT_OBJECT_0 + g_slot_count) {
            continue;
        }
        u32 idx = (u32)(w - WAIT_OBJECT_0 - 1u);
        WatchSlot *slot = &g_slots[idx];
        DWORD transferred = 0;
        if (!GetOverlappedResult(slot->dir, &slot->ov, &transferred, FALSE)) {
            transferred = 0;
        }
        (void)transferred;

        ResetEvent(slot->ov.hEvent);
        DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
            | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        if (!ReadDirectoryChangesW(slot->dir, slot->buffer, CATALOG_WATCH_NOTIFY_BUF, TRUE, filter, NULL, &slot->ov, NULL)) {
            debug_log_wide(L"ReadDirectoryChangesW failed for watch slot %u", (unsigned int)idx);
        }

        if (g_notify_hwnd && IsWindow(g_notify_hwnd)) {
            PostMessageW(g_notify_hwnd, WM_LAUNCHER_CATALOG_FS_CHANGED, 0, 0);
        }
    }

    return 0;
}

void
catalog_watch_start(HWND hwnd, const wchar_t *install_dir)
{
    catalog_watch_stop();

    if (!hwnd || !install_dir || !install_dir[0]) {
        return;
    }

    CatalogWatchRootDirs roots = {0};
    if (!catalog_watch_root_dirs_collect(install_dir, &roots) || roots.count == 0) {
        catalog_watch_root_dirs_free(&roots);
        return;
    }

    u32 n = roots.count;
    if (n > CATALOG_WATCH_MAX_DIRS) {
        debug_log_wide(L"catalog watch: capping %u roots to %u (WaitForMultipleObjects limit)", (unsigned int)n,
                       (unsigned int)CATALOG_WATCH_MAX_DIRS);
        n = CATALOG_WATCH_MAX_DIRS;
    }

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop_event) {
        catalog_watch_root_dirs_free(&roots);
        return;
    }

    g_slots = (WatchSlot *)heap_alloc_zero(sizeof(WatchSlot) * n);
    if (!g_slots) {
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
        catalog_watch_root_dirs_free(&roots);
        return;
    }

    g_slot_count = 0;
    for (u32 i = 0; i < n; ++i) {
        const wchar_t *path = roots.paths[i];
        if (!path || !path[0]) {
            continue;
        }
        WatchSlot *slot = &g_slots[g_slot_count];
        ZeroMemory(slot, sizeof(*slot));
        slot->dir = CreateFileW(path, FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        if (slot->dir == INVALID_HANDLE_VALUE) {
            debug_log_wide(L"catalog watch: skip (cannot open) %ls", path);
            continue;
        }
        slot->ov.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!slot->ov.hEvent) {
            CloseHandle(slot->dir);
            slot->dir = INVALID_HANDLE_VALUE;
            continue;
        }
        DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
            | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        if (!ReadDirectoryChangesW(slot->dir, slot->buffer, CATALOG_WATCH_NOTIFY_BUF, TRUE, filter, NULL, &slot->ov, NULL)) {
            debug_log_wide(L"catalog watch: ReadDirectoryChangesW failed for %ls", path);
            CloseHandle(slot->ov.hEvent);
            slot->ov.hEvent = NULL;
            CloseHandle(slot->dir);
            slot->dir = INVALID_HANDLE_VALUE;
            continue;
        }
        g_slot_count++;
    }

    catalog_watch_root_dirs_free(&roots);

    if (g_slot_count == 0) {
        heap_free(g_slots);
        g_slots = NULL;
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
        return;
    }

    g_wait_handles = (HANDLE *)heap_alloc_zero(sizeof(HANDLE) * ((size_t)g_slot_count + 1u));
    if (!g_wait_handles) {
        catalog_watch_stop();
        return;
    }

    g_notify_hwnd = hwnd;
    g_thread = CreateThread(NULL, 0, catalog_watch_thread_main, NULL, 0, NULL);
    if (!g_thread) {
        catalog_watch_stop();
    }
}

void
catalog_watch_stop(void)
{
    if (g_stop_event) {
        SetEvent(g_stop_event);
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
    }
    for (u32 i = 0; i < g_slot_count; ++i) {
        if (g_slots[i].ov.hEvent) {
            CloseHandle(g_slots[i].ov.hEvent);
        }
        if (g_slots[i].dir && g_slots[i].dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(g_slots[i].dir, &g_slots[i].ov);
            CloseHandle(g_slots[i].dir);
        }
    }
    heap_free(g_slots);
    g_slots = NULL;
    heap_free(g_wait_handles);
    g_wait_handles = NULL;
    g_slot_count = 0;
    g_notify_hwnd = NULL;
}
