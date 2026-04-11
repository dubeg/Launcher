#include "fuzzy.h"

#include "../core/base.h"
#include "../../third_party/fzy/match.h"

#include <stdlib.h>
#include <string.h>

typedef struct SortContext {
    SearchResult *items;
} SortContext;

static int
compare_results(const void *a, const void *b)
{
    const SearchResult *left = (const SearchResult *)a;
    const SearchResult *right = (const SearchResult *)b;
    if (left->score < right->score) {
        return 1;
    }
    if (left->score > right->score) {
        return -1;
    }
    return strcmp(left->item->display_name, right->item->display_name);
}

FuzzyMatch
fuzzy_score_text(const char *query, const char *candidate)
{
    FuzzyMatch result = {0};
    if (!query || !query[0]) {
        result.matched = true;
        result.score = 0.0;
        return result;
    }

    if (!has_match(query, candidate)) {
        return result;
    }

    result.matched = true;
    result.score = match(query, candidate);
    return result;
}

SearchResultArray
fuzzy_rank_items(Arena *arena, const char *query, const LaunchItem *items, u32 item_count, u32 max_results)
{
    SearchResultArray results = {0};
    if (!item_count) {
        return results;
    }

    SearchResult *scratch = (SearchResult *)arena_push_zero(arena, sizeof(SearchResult) * item_count, sizeof(void *));
    u32 count = 0;

    for (u32 i = 0; i < item_count; ++i) {
        const LaunchItem *item = &items[i];
        SearchResult candidate = {0};
        candidate.item = item;

        if (query && query[0]) {
            FuzzyMatch primary = fuzzy_score_text(query, item->search_text);
            if (!primary.matched && item->subtitle) {
                primary = fuzzy_score_text(query, item->subtitle);
                primary.score -= 0.25;
            }
            if (!primary.matched) {
                continue;
            }
            candidate.score = primary.score;
        } else {
            candidate.score = 0.0;
        }

        if (item->source == LaunchSource_StartMenu || item->source == LaunchSource_StartMenuShortcut) {
            candidate.score += 0.25;
        }
        if (item->source == LaunchSource_System32) {
            candidate.score -= 0.10;
        }
        scratch[count++] = candidate;
    }

    qsort(scratch, count, sizeof(SearchResult), compare_results);

    if (count > max_results) {
        count = max_results;
    }

    results.items = (SearchResult *)arena_push(arena, sizeof(SearchResult) * count, sizeof(void *));
    memcpy(results.items, scratch, sizeof(SearchResult) * count);
    results.count = count;
    return results;
}
