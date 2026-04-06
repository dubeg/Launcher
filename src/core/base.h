#ifndef LAUNCHER_BASE_H
#define LAUNCHER_BASE_H

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef CINTERFACE
#define CINTERFACE
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

#define array_count(a) (sizeof(a) / sizeof((a)[0]))
#define kilobytes(v) ((u64)(v) * 1024ULL)
#define megabytes(v) (kilobytes(v) * 1024ULL)
#define gigabytes(v) (megabytes(v) * 1024ULL)
#define align_pow2(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

typedef struct Str8 {
    char *data;
    size_t size;
} Str8;

typedef struct Arena {
    u8 *base;
    u64 reserve_size;
    u64 commit_size;
    u64 pos;
    u64 commit_granularity;
} Arena;

typedef struct ArenaTemp {
    Arena *arena;
    u64 pos;
} ArenaTemp;

typedef struct FileData {
    void *data;
    size_t size;
} FileData;

void fatal_win32(const wchar_t *context);
void fatal_message(const wchar_t *message);

Arena arena_create(u64 reserve_size, u64 initial_commit_size);
void arena_destroy(Arena *arena);
void arena_reset(Arena *arena);
void *arena_push(Arena *arena, u64 size, u64 alignment);
void *arena_push_zero(Arena *arena, u64 size, u64 alignment);
ArenaTemp arena_temp_begin(Arena *arena);
void arena_temp_end(ArenaTemp temp);
char *arena_strdup(Arena *arena, const char *text);
char *arena_strndup(Arena *arena, const char *text, size_t size);
wchar_t *arena_wcsdup(Arena *arena, const wchar_t *text);

void *heap_alloc_zero(size_t size);
void *heap_realloc(void *ptr, size_t size);
void heap_free(void *ptr);

FileData read_entire_file_wide(const wchar_t *path);
void free_file_data(FileData *file);
char *utf8_from_wide(Arena *arena, const wchar_t *text);
wchar_t *wide_from_utf8(Arena *arena, const char *text);
void utf8_from_wide_buffer(const wchar_t *text, char *buffer, size_t buffer_size);
void wide_from_utf8_buffer(const char *text, wchar_t *buffer, size_t buffer_size);
void lowercase_ascii_in_place(char *text);
bool ascii_case_contains(const char *haystack, const char *needle);
bool path_exists_wide(const wchar_t *path);
char *path_filename_utf8(const char *path);
char *wide_path_filename_utf8(Arena *arena, const wchar_t *path);
void debug_log_wide(const wchar_t *format, ...);

#endif
