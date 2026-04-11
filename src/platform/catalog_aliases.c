#include "catalog_aliases.h"

#include <stdio.h>
#include <string.h>

static bool
filename_utf8_is_msc_or_cpl(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    return _stricmp(dot, ".msc") == 0 || _stricmp(dot, ".cpl") == 0;
}

void
catalog_aliases_load_json(Arena *arena, const wchar_t *path, CatalogAliases *out)
{
    out->entries = NULL;
    out->count = 0;

    FileData file = read_entire_file_wide(path);
    if (!file.data) {
        return;
    }

    char *text = (char *)file.data;
    u32 capacity = 32;
    CatalogAliasEntry *entries = (CatalogAliasEntry *)arena_push_zero(arena, sizeof(CatalogAliasEntry) * capacity, sizeof(void *));
    u32 count = 0;
    char *cursor = text;
    while ((cursor = strchr(cursor, '"')) != NULL) {
        char *key_start = cursor + 1;
        char *key_end = strchr(key_start, '"');
        if (!key_end) {
            break;
        }
        cursor = strchr(key_end, ':');
        if (!cursor) {
            break;
        }
        cursor = strchr(cursor, '"');
        if (!cursor) {
            break;
        }
        char *value_start = cursor + 1;
        char *value_end = strchr(value_start, '"');
        if (!value_end) {
            break;
        }

        if (count >= capacity) {
            u32 old_capacity = capacity;
            capacity *= 2;
            CatalogAliasEntry *new_entries = (CatalogAliasEntry *)arena_push_zero(arena, sizeof(CatalogAliasEntry) * capacity, sizeof(void *));
            memcpy(new_entries, entries, sizeof(CatalogAliasEntry) * old_capacity);
            entries = new_entries;
        }
        entries[count].key = arena_strndup(arena, key_start, (size_t)(key_end - key_start));
        lowercase_ascii_in_place(entries[count].key);
        entries[count].friendly_name = arena_strndup(arena, value_start, (size_t)(value_end - value_start));
        ++count;
        cursor = value_end + 1;
    }

    free_file_data(&file);
    out->entries = entries;
    out->count = count;
}

const char *
catalog_aliases_lookup_msc_cpl(const CatalogAliases *aliases, const char *filename_lower_utf8)
{
    if (!aliases || !aliases->count || !filename_lower_utf8 || !filename_utf8_is_msc_or_cpl(filename_lower_utf8)) {
        return NULL;
    }
    for (u32 i = 0; i < aliases->count; ++i) {
        if (_stricmp(aliases->entries[i].key, filename_lower_utf8) == 0) {
            return aliases->entries[i].friendly_name;
        }
    }
    return NULL;
}
