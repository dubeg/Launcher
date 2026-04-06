#include "base.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static u64
arena_commit_target(u64 value, u64 granularity)
{
    return align_pow2(value, granularity);
}

void
fatal_win32(const wchar_t *context)
{
    DWORD error = GetLastError();
    wchar_t system_message[1024];
    DWORD written = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        0,
        system_message,
        array_count(system_message),
        NULL);

    wchar_t message[1400];
    if (written > 0) {
        _snwprintf_s(message, array_count(message), _TRUNCATE, L"%ls failed with error %lu:\n%ls", context, (unsigned long)error, system_message);
    } else {
        _snwprintf_s(message, array_count(message), _TRUNCATE, L"%ls failed with error %lu.", context, (unsigned long)error);
    }
    MessageBoxW(NULL, message, L"Launcher Error", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

void
fatal_message(const wchar_t *message)
{
    MessageBoxW(NULL, message, L"Launcher Error", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

Arena
arena_create(u64 reserve_size, u64 initial_commit_size)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);

    Arena arena = {0};
    arena.commit_granularity = info.dwPageSize;
    arena.reserve_size = align_pow2(reserve_size, info.dwAllocationGranularity);
    arena.commit_size = arena_commit_target(initial_commit_size, arena.commit_granularity);

    arena.base = (u8 *)VirtualAlloc(NULL, arena.reserve_size, MEM_RESERVE, PAGE_READWRITE);
    if (!arena.base) {
        fatal_win32(L"VirtualAlloc reserve");
    }
    if (arena.commit_size > 0) {
        if (!VirtualAlloc(arena.base, arena.commit_size, MEM_COMMIT, PAGE_READWRITE)) {
            fatal_win32(L"VirtualAlloc commit");
        }
    }

    return arena;
}

void
arena_destroy(Arena *arena)
{
    if (arena && arena->base) {
        VirtualFree(arena->base, 0, MEM_RELEASE);
        ZeroMemory(arena, sizeof(*arena));
    }
}

void
arena_reset(Arena *arena)
{
    if (arena) {
        arena->pos = 0;
    }
}

static void
arena_ensure(Arena *arena, u64 size)
{
    if (size <= arena->commit_size) {
        return;
    }

    u64 new_commit = arena_commit_target(size, arena->commit_granularity);
    if (new_commit > arena->reserve_size) {
        fatal_message(L"Arena capacity exceeded.");
    }

    if (!VirtualAlloc(arena->base + arena->commit_size, new_commit - arena->commit_size, MEM_COMMIT, PAGE_READWRITE)) {
        fatal_win32(L"VirtualAlloc grow");
    }
    arena->commit_size = new_commit;
}

void *
arena_push(Arena *arena, u64 size, u64 alignment)
{
    if (alignment == 0) {
        alignment = 1;
    }
    u64 aligned_pos = align_pow2(arena->pos, alignment);
    u64 new_pos = aligned_pos + size;
    arena_ensure(arena, new_pos);

    void *result = arena->base + aligned_pos;
    arena->pos = new_pos;
    return result;
}

void *
arena_push_zero(Arena *arena, u64 size, u64 alignment)
{
    void *result = arena_push(arena, size, alignment);
    ZeroMemory(result, (SIZE_T)size);
    return result;
}

ArenaTemp
arena_temp_begin(Arena *arena)
{
    ArenaTemp temp;
    temp.arena = arena;
    temp.pos = arena->pos;
    return temp;
}

void
arena_temp_end(ArenaTemp temp)
{
    temp.arena->pos = temp.pos;
}

char *
arena_strndup(Arena *arena, const char *text, size_t size)
{
    char *copy = (char *)arena_push(arena, size + 1, 1);
    memcpy(copy, text, size);
    copy[size] = 0;
    return copy;
}

char *
arena_strdup(Arena *arena, const char *text)
{
    return arena_strndup(arena, text, strlen(text));
}

wchar_t *
arena_wcsdup(Arena *arena, const wchar_t *text)
{
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)arena_push(arena, (length + 1) * sizeof(wchar_t), sizeof(wchar_t));
    memcpy(copy, text, (length + 1) * sizeof(wchar_t));
    return copy;
}

void *
heap_alloc_zero(size_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void *
heap_realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    }
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ptr, size);
}

void
heap_free(void *ptr)
{
    if (ptr) {
        HeapFree(GetProcessHeap(), 0, ptr);
    }
}

FileData
read_entire_file_wide(const wchar_t *path)
{
    FileData result = {0};
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return result;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return result;
    }

    result.size = (size_t)size.QuadPart;
    result.data = heap_alloc_zero(result.size + 1);
    if (!result.data) {
        CloseHandle(file);
        result.size = 0;
        return result;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(file, result.data, (DWORD)result.size, &bytes_read, NULL)) {
        free_file_data(&result);
    }
    CloseHandle(file);
    return result;
}

void
free_file_data(FileData *file)
{
    if (file->data) {
        heap_free(file->data);
        file->data = NULL;
        file->size = 0;
    }
}

char *
utf8_from_wide(Arena *arena, const wchar_t *text)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *buffer = (char *)arena_push(arena, (u64)length, 1);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, length, NULL, NULL);
    return buffer;
}

wchar_t *
wide_from_utf8(Arena *arena, const char *text)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *buffer = (wchar_t *)arena_push(arena, (u64)length * sizeof(wchar_t), sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, length);
    return buffer;
}

void
utf8_from_wide_buffer(const wchar_t *text, char *buffer, size_t buffer_size)
{
    if (!buffer_size) {
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, (int)buffer_size, NULL, NULL);
    buffer[buffer_size - 1] = 0;
}

void
wide_from_utf8_buffer(const char *text, wchar_t *buffer, size_t buffer_size)
{
    if (!buffer_size) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, (int)buffer_size);
    buffer[buffer_size - 1] = 0;
}

void
lowercase_ascii_in_place(char *text)
{
    for (; *text; ++text) {
        if (*text >= 'A' && *text <= 'Z') {
            *text = (char)(*text - 'A' + 'a');
        }
    }
}

bool
ascii_case_contains(const char *haystack, const char *needle)
{
    if (!needle[0]) {
        return true;
    }
    size_t hay_len = strlen(haystack);
    size_t needle_len = strlen(needle);
    if (needle_len > hay_len) {
        return false;
    }

    for (size_t i = 0; i + needle_len <= hay_len; ++i) {
        bool match = true;
        for (size_t j = 0; j < needle_len; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool
path_exists_wide(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

char *
path_filename_utf8(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && slash > base) {
        base = slash + 1;
    }
    if (backslash && backslash + 1 > base) {
        base = backslash + 1;
    }
    return (char *)base;
}

char *
wide_path_filename_utf8(Arena *arena, const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'/');
    const wchar_t *backslash = wcsrchr(path, L'\\');
    const wchar_t *base = path;
    if (slash && slash > base) {
        base = slash + 1;
    }
    if (backslash && backslash + 1 > base) {
        base = backslash + 1;
    }
    return utf8_from_wide(arena, base);
}

void
debug_log_wide(const wchar_t *format, ...)
{
    wchar_t path[MAX_PATH];
    DWORD size = GetTempPathW(array_count(path), path);
    if (size == 0 || size >= array_count(path)) {
        return;
    }

    wcscat_s(path, array_count(path), L"launcher-debug.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, array_count(buffer), _TRUNCATE, format, args);
    va_end(args);
    wcscat_s(buffer, array_count(buffer), L"\r\n");

    char utf8[4096];
    utf8_from_wide_buffer(buffer, utf8, array_count(utf8));
    DWORD written = 0;
    WriteFile(file, utf8, (DWORD)strlen(utf8), &written, NULL);
    CloseHandle(file);
}
