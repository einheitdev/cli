/// @file fuzzy.h
/// @brief Fuzzy string matching for "did you mean X?" suggestions.
///
/// Used by the command tree when a verb typo misses dispatch and by
/// the schema validator when a set-path field name is slightly off.
/// Levenshtein distance with early termination; cheap enough to run
/// on every rejected token.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_FUZZY_H_
#define INCLUDE_EINHEIT_CLI_FUZZY_H_

#include <cstddef>
#include <string>
#include <vector>

namespace einheit::cli::fuzzy {

/// Compute Levenshtein edit distance between `a` and `b`.
/// @param a First string.
/// @param b Second string.
/// @returns Number of single-character edits between them.
auto Distance(const std::string &a, const std::string &b)
    -> std::size_t;

/// Suggest candidates from `vocabulary` that are close to `query`.
/// Threshold scales with query length so long words tolerate more
/// typos. Empty result means no close match was found.
/// @param query The typed token.
/// @param vocabulary Known strings to match against.
/// @returns Sorted candidates, best match first.
auto Suggest(const std::string &query,
             const std::vector<std::string> &vocabulary)
    -> std::vector<std::string>;

}  // namespace einheit::cli::fuzzy

#endif  // INCLUDE_EINHEIT_CLI_FUZZY_H_
