#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace stremio::archive {
struct MatchState;
// Position in the selected immutable sort array, never a SQL row offset.
// State pins the exact predicates and reader generation used by the first page.
struct Cursor {
    std::shared_ptr<const MatchState> matches;
    uint64_t generation = 0;
    size_t position = 0;
    int sort = 0;
    bool descending = true;
};
struct QueryMetrics {
    double filterMs = 0, searchMs = 0, sortMs = 0, metadataMs = 0, totalMs = 0;
    size_t facetWords = 0, searchPostings = 0, searchCandidates = 0, sortEntries = 0, metadataRows = 0;
    size_t sqlFullScanSteps = 0, sqlSorts = 0;
    bool reusedMatches = false;
};
} // namespace stremio::archive
