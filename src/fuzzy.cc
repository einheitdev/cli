/// @file fuzzy.cc
/// @brief Levenshtein distance + "did you mean" suggestion engine.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/fuzzy.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace einheit::cli::fuzzy {
namespace {

// Threshold for "close enough to suggest". Shorter strings get
// tighter tolerance: 1 edit for <= 4 chars, 2 for <= 8, else 3.
auto ThresholdFor(std::size_t len) -> std::size_t {
  if (len <= 4) return 1;
  if (len <= 8) return 2;
  return 3;
}

}  // namespace

auto Distance(const std::string &a, const std::string &b)
    -> std::size_t {
  const std::size_t na = a.size();
  const std::size_t nb = b.size();
  if (na == 0) return nb;
  if (nb == 0) return na;

  // Two-row rolling DP. Index i = position in `a`, j = in `b`.
  std::vector<std::size_t> prev(nb + 1);
  std::vector<std::size_t> cur(nb + 1);
  for (std::size_t j = 0; j <= nb; ++j) prev[j] = j;

  for (std::size_t i = 1; i <= na; ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= nb; ++j) {
      const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({
          cur[j - 1] + 1,        // insertion
          prev[j] + 1,           // deletion
          prev[j - 1] + cost,    // substitution
      });
    }
    std::swap(prev, cur);
  }
  return prev[nb];
}

auto Suggest(const std::string &query,
             const std::vector<std::string> &vocabulary)
    -> std::vector<std::string> {
  const std::size_t threshold = ThresholdFor(query.size());

  // Score = (not_a_prefix, edit_distance). Prefix matches sort
  // first because typing an unambiguous shorthand ("config") for a
  // longer command ("configure") is the most common case — pure
  // edit distance penalises it disproportionately.
  struct Entry {
    int prefix_score;
    std::size_t distance;
    std::string word;
  };
  std::vector<Entry> scored;
  for (const auto &word : vocabulary) {
    if (word == query) continue;
    const bool is_prefix =
        !query.empty() && word.size() >= query.size() &&
        word.compare(0, query.size(), query) == 0;
    const auto d = Distance(query, word);
    if (is_prefix) {
      scored.push_back({0, d, word});
    } else if (d <= threshold) {
      scored.push_back({1, d, word});
    }
  }
  std::sort(scored.begin(), scored.end(),
            [](const Entry &lhs, const Entry &rhs) {
              if (lhs.prefix_score != rhs.prefix_score) {
                return lhs.prefix_score < rhs.prefix_score;
              }
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              return lhs.word < rhs.word;
            });

  std::vector<std::string> out;
  out.reserve(scored.size());
  for (auto &e : scored) out.push_back(std::move(e.word));
  return out;
}

}  // namespace einheit::cli::fuzzy
