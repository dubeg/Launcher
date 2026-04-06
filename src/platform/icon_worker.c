#include "icon_worker.h"

#include <shobjidl.h>
#include <shellapi.h>
#include <math.h>

static bool
resample_rgba_area_premul(const u8 *src, s32 src_w, s32 src_h, u8 *dst, s32 dst_w, s32 dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

    f32 scale_x = (f32)src_w / (f32)dst_w;
    f32 scale_y = (f32)src_h / (f32)dst_h;
    for (s32 y = 0; y < dst_h; ++y) {
        f32 y0f = (f32)y * scale_y;
        f32 y1f = (f32)(y + 1) * scale_y;
        s32 sy0 = (s32)floorf(y0f);
        s32 sy1 = (s32)ceilf(y1f);
        if (sy0 < 0) {
            sy0 = 0;
        }
        if (sy1 > src_h) {
            sy1 = src_h;
        }
        for (s32 x = 0; x < dst_w; ++x) {
            f32 x0f = (f32)x * scale_x;
            f32 x1f = (f32)(x + 1) * scale_x;
            s32 sx0 = (s32)floorf(x0f);
            s32 sx1 = (s32)ceilf(x1f);
            if (sx0 < 0) {
                sx0 = 0;
            }
            if (sx1 > src_w) {
                sx1 = src_w;
            }

            f32 acc_a = 0.0f;
            f32 acc_rp = 0.0f;
            f32 acc_gp = 0.0f;
            f32 acc_bp = 0.0f;
            f32 total_w = 0.0f;
            for (s32 sy = sy0; sy < sy1; ++sy) {
                f32 py0 = (f32)sy;
                f32 py1 = (f32)(sy + 1);
                f32 wy0 = y0f > py0 ? y0f : py0;
                f32 wy1 = y1f < py1 ? y1f : py1;
                f32 wy = wy1 - wy0;
                if (wy <= 0.0f) {
                    continue;
                }
                for (s32 sx = sx0; sx < sx1; ++sx) {
                    f32 px0 = (f32)sx;
                    f32 px1 = (f32)(sx + 1);
                    f32 wx0 = x0f > px0 ? x0f : px0;
                    f32 wx1 = x1f < px1 ? x1f : px1;
                    f32 wx = wx1 - wx0;
                    if (wx <= 0.0f) {
                        continue;
                    }
                    f32 w = wx * wy;
                    const u8 *p = src + ((size_t)sy * (size_t)src_w + (size_t)sx) * 4u;
                    f32 a = (f32)p[3] / 255.0f;
                    acc_a += a * w;
                    acc_rp += ((f32)p[0] * a) * w;
                    acc_gp += ((f32)p[1] * a) * w;
                    acc_bp += ((f32)p[2] * a) * w;
                    total_w += w;
                }
            }

            f32 a = 0.0f;
            f32 rp = 0.0f;
            f32 gp = 0.0f;
            f32 bp = 0.0f;
            if (total_w > 0.00001f) {
                a = acc_a / total_w;
                rp = acc_rp / total_w;
                gp = acc_gp / total_w;
                bp = acc_bp / total_w;
            }

            u8 *out = dst + ((size_t)y * (size_t)dst_w + (size_t)x) * 4u;
            out[3] = (u8)(a * 255.0f + 0.5f);
            if (a > 0.0001f) {
                out[0] = (u8)(rp / a + 0.5f);
                out[1] = (u8)(gp / a + 0.5f);
                out[2] = (u8)(bp / a + 0.5f);
            } else {
                out[0] = 0;
                out[1] = 0;
                out[2] = 0;
            }
        }
    }
    return true;
}

static bool
extract_shell_item_image_rgba(const wchar_t *path, s32 source_size, u8 **out_pixels, s32 *out_width, s32 *out_height, s32 *out_stride)
{
    if (!path || !path[0] || !out_pixels || !out_width || !out_height || !out_stride || source_size <= 0) {
        return false;
    }

    IShellItemImageFactory *factory = NULL;
    HRESULT hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void **)&factory);
    if (FAILED(hr) || !factory) {
        return false;
    }

    SIZE size = {source_size, source_size};
    HBITMAP hbmp = NULL;
    hr = IShellItemImageFactory_GetImage(factory, size, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &hbmp);
    IShellItemImageFactory_Release(factory);
    if (FAILED(hr) || !hbmp) {
        return false;
    }

    BITMAP bm;
    ZeroMemory(&bm, sizeof(bm));
    if (!GetObjectW(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(hbmp);
        return false;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) {
        DeleteObject(hbmp);
        return false;
    }

    s32 stride = bm.bmWidth * 4;
    u8 *bgra = (u8 *)heap_alloc_zero((size_t)stride * (size_t)bm.bmHeight);
    bool ok = false;
    if (bgra && GetDIBits(hdc, hbmp, 0, (UINT)bm.bmHeight, bgra, &bmi, DIB_RGB_COLORS)) {
        u8 *rgba = (u8 *)heap_alloc_zero((size_t)stride * (size_t)bm.bmHeight);
        if (rgba) {
            for (s32 y = 0; y < bm.bmHeight; ++y) {
                for (s32 x = 0; x < bm.bmWidth; ++x) {
                    size_t p = (size_t)(y * stride + x * 4);
                    rgba[p + 0] = bgra[p + 2];
                    rgba[p + 1] = bgra[p + 1];
                    rgba[p + 2] = bgra[p + 0];
                    rgba[p + 3] = bgra[p + 3];
                }
            }
            *out_pixels = rgba;
            *out_width = bm.bmWidth;
            *out_height = bm.bmHeight;
            *out_stride = stride;
            ok = true;
        }
    }

    heap_free(bgra);
    DeleteDC(hdc);
    DeleteObject(hbmp);
    return ok;
}

static bool
extract_shell_icon_rgba(const wchar_t *path, s32 icon_index, s32 icon_size, u8 **out_pixels, s32 *out_width, s32 *out_height, s32 *out_stride)
{
    if (!path || !path[0] || !out_pixels || !out_width || !out_height || !out_stride || icon_size <= 0) {
        return false;
    }

    SHFILEINFOW info;
    ZeroMemory(&info, sizeof(info));
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (icon_index >= 0) {
        flags |= SHGFI_SYSICONINDEX;
    }
    if (!SHGetFileInfoW(path, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags)) {
        ZeroMemory(&info, sizeof(info));
        flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
        if (!SHGetFileInfoW(path, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags)) {
            return false;
        }
    }
    if (!info.hIcon) {
        return false;
    }

    s32 source_size = icon_size * 3;
    if (source_size < 48) {
        source_size = 48;
    }
    if (source_size > 128) {
        source_size = 128;
    }

    u8 *native_rgba = NULL;
    s32 native_w = 0;
    s32 native_h = 0;
    s32 native_stride = 0;
    if (extract_shell_item_image_rgba(path, source_size, &native_rgba, &native_w, &native_h, &native_stride)) {
        u8 *pixels = (u8 *)heap_alloc_zero((size_t)icon_size * (size_t)icon_size * 4u);
        if (pixels) {
            resample_rgba_area_premul(native_rgba, native_w, native_h, pixels, icon_size, icon_size);
            *out_pixels = pixels;
            *out_width = icon_size;
            *out_height = icon_size;
            *out_stride = icon_size * 4;
            heap_free(native_rgba);
            DestroyIcon(info.hIcon);
            return true;
        }
        heap_free(native_rgba);
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = source_size;
    bmi.bmiHeader.biHeight = -source_size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) {
        DestroyIcon(info.hIcon);
        return false;
    }
    void *dib_pixels = NULL;
    HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dib_pixels, NULL, 0);
    if (!dib || !dib_pixels) {
        DeleteDC(hdc);
        DestroyIcon(info.hIcon);
        return false;
    }

    HGDIOBJ old_obj = SelectObject(hdc, dib);
    PatBlt(hdc, 0, 0, source_size, source_size, BLACKNESS);
    DrawIconEx(hdc, 0, 0, info.hIcon, source_size, source_size, 0, NULL, DI_NORMAL);

    s32 src_stride = source_size * 4;
    u8 *src_rgba = (u8 *)heap_alloc_zero((size_t)src_stride * (size_t)source_size);
    u8 *pixels = NULL;
    if (src_rgba) {
        const u8 *src = (const u8 *)dib_pixels;
        for (s32 y = 0; y < source_size; ++y) {
            for (s32 x = 0; x < source_size; ++x) {
                size_t p = (size_t)(y * src_stride + x * 4);
                src_rgba[p + 0] = src[p + 2];
                src_rgba[p + 1] = src[p + 1];
                src_rgba[p + 2] = src[p + 0];
                src_rgba[p + 3] = src[p + 3];
            }
        }
        pixels = (u8 *)heap_alloc_zero((size_t)icon_size * (size_t)icon_size * 4u);
        if (pixels && resample_rgba_area_premul(src_rgba, source_size, source_size, pixels, icon_size, icon_size)) {
            *out_pixels = pixels;
            *out_width = icon_size;
            *out_height = icon_size;
            *out_stride = icon_size * 4;
            pixels = NULL;
        }
    }

    heap_free(src_rgba);
    heap_free(pixels);
    SelectObject(hdc, old_obj);
    DeleteObject(dib);
    DeleteDC(hdc);
    DestroyIcon(info.hIcon);
    return (*out_pixels != NULL);
}

static DWORD WINAPI
icon_worker_thread_proc(void *param)
{
    IconWorker *worker = (IconWorker *)param;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    for (;;) {
        IconWorkerRequest request;
        bool have_request = false;

        EnterCriticalSection(&worker->lock);
        while (!worker->stop_requested && worker->req_read == worker->req_write) {
            SleepConditionVariableCS(&worker->cv, &worker->lock, INFINITE);
        }
        if (!worker->stop_requested && worker->req_read != worker->req_write) {
            request = worker->requests[worker->req_read];
            worker->req_read = (worker->req_read + 1) % ICON_WORKER_REQUEST_CAPACITY;
            have_request = true;
        }
        bool should_stop = worker->stop_requested;
        LeaveCriticalSection(&worker->lock);

        if (should_stop && !have_request) {
            break;
        }
        if (!have_request) {
            continue;
        }

        IconWorkerResult result;
        ZeroMemory(&result, sizeof(result));
        result.entry_index = request.entry_index;
        result.generation = request.generation;
        result.success = extract_shell_icon_rgba(request.path, request.icon_index, request.icon_size,
                                                 &result.pixels, &result.width, &result.height, &result.stride);

        EnterCriticalSection(&worker->lock);
        s32 next_write = (worker->done_write + 1) % ICON_WORKER_COMPLETED_CAPACITY;
        if (next_write == worker->done_read) {
            icon_worker_free_result(&result);
        } else {
            worker->completed[worker->done_write] = result;
            worker->done_write = next_write;
        }
        LeaveCriticalSection(&worker->lock);
    }

    CoUninitialize();
    return 0;
}

bool
icon_worker_init(IconWorker *worker)
{
    if (!worker) {
        return false;
    }
    ZeroMemory(worker, sizeof(*worker));
    InitializeCriticalSection(&worker->lock);
    InitializeConditionVariable(&worker->cv);
    worker->thread = CreateThread(NULL, 0, icon_worker_thread_proc, worker, 0, NULL);
    if (!worker->thread) {
        DeleteCriticalSection(&worker->lock);
        ZeroMemory(worker, sizeof(*worker));
        return false;
    }
    return true;
}

void
icon_worker_shutdown(IconWorker *worker)
{
    if (!worker) {
        return;
    }
    EnterCriticalSection(&worker->lock);
    worker->stop_requested = true;
    WakeAllConditionVariable(&worker->cv);
    LeaveCriticalSection(&worker->lock);

    if (worker->thread) {
        WaitForSingleObject(worker->thread, INFINITE);
        CloseHandle(worker->thread);
        worker->thread = NULL;
    }

    for (s32 i = worker->done_read; i != worker->done_write; i = (i + 1) % ICON_WORKER_COMPLETED_CAPACITY) {
        icon_worker_free_result(&worker->completed[i]);
    }
    DeleteCriticalSection(&worker->lock);
    ZeroMemory(worker, sizeof(*worker));
}

bool
icon_worker_submit(IconWorker *worker, const IconWorkerRequest *request)
{
    if (!worker || !request) {
        return false;
    }
    bool ok = false;
    EnterCriticalSection(&worker->lock);
    s32 next_write = (worker->req_write + 1) % ICON_WORKER_REQUEST_CAPACITY;
    if (next_write != worker->req_read) {
        worker->requests[worker->req_write] = *request;
        worker->req_write = next_write;
        ok = true;
        WakeConditionVariable(&worker->cv);
    }
    LeaveCriticalSection(&worker->lock);
    return ok;
}

bool
icon_worker_take_completed(IconWorker *worker, IconWorkerResult *out_result)
{
    if (!worker || !out_result) {
        return false;
    }
    bool ok = false;
    EnterCriticalSection(&worker->lock);
    if (worker->done_read != worker->done_write) {
        *out_result = worker->completed[worker->done_read];
        worker->done_read = (worker->done_read + 1) % ICON_WORKER_COMPLETED_CAPACITY;
        ok = true;
    }
    LeaveCriticalSection(&worker->lock);
    return ok;
}

void
icon_worker_free_result(IconWorkerResult *result)
{
    if (!result) {
        return;
    }
    if (result->pixels) {
        heap_free(result->pixels);
    }
    ZeroMemory(result, sizeof(*result));
}
