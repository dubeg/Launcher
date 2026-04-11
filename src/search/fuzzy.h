#ifndef LAUNCHER_FUZZY_H
#define LAUNCHER_FUZZY_H

#include "../app/app.h"

typedef struct FuzzyMatch {
    bool matched;
    double score;
} FuzzyMatch;

FuzzyMatch fuzzy_score_text(const char *query, const char *candidate);
SearchResultArray fuzzy_rank_items(Arena *arena, const char *query, const LaunchItem *items, u32 item_count, u32 max_results);

/* Preserve item order; each score 0 (for pre-filtered result sets such as Everything globs). */
SearchResultArray fuzzy_pass_through_items(Arena *arena, const LaunchItem *items, u32 item_count, u32 max_results);

#endif
