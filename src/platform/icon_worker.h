#ifndef LAUNCHER_ICON_WORKER_H
#define LAUNCHER_ICON_WORKER_H

#include "../core/base.h"

#define ICON_WORKER_PATH_CAPACITY (MAX_PATH * 4)
#define ICON_WORKER_REQUEST_CAPACITY 1024
#define ICON_WORKER_COMPLETED_CAPACITY 1024

typedef struct IconWorkerRequest {
    s32 entry_index;
    u32 generation;
    s32 icon_index;
    s32 icon_size;
    wchar_t path[ICON_WORKER_PATH_CAPACITY];
    wchar_t path_fallback[ICON_WORKER_PATH_CAPACITY];
    s32 icon_index_fallback;
} IconWorkerRequest;

typedef struct IconWorkerResult {
    s32 entry_index;
    u32 generation;
    bool success;
    u8 *pixels;
    s32 width;
    s32 height;
    s32 stride;
} IconWorkerResult;

typedef struct IconWorker {
    HANDLE thread;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
    bool stop_requested;
    IconWorkerRequest requests[ICON_WORKER_REQUEST_CAPACITY];
    s32 req_read;
    s32 req_write;
    IconWorkerResult completed[ICON_WORKER_COMPLETED_CAPACITY];
    s32 done_read;
    s32 done_write;
} IconWorker;

bool icon_worker_init(IconWorker *worker);
void icon_worker_shutdown(IconWorker *worker);
bool icon_worker_submit(IconWorker *worker, const IconWorkerRequest *request);
bool icon_worker_take_completed(IconWorker *worker, IconWorkerResult *out_result);
void icon_worker_free_result(IconWorkerResult *result);

#endif
