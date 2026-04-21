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

  std::vector<std::pair<std::size_t, std::string>> scored;
  for (const auto &word : vocabulary) {
    if (word == query) continue;
    const auto d = Distance(query, word);
    if (d <= threshold) scored.emplace_back(d, word);
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.first != rhs.first) return lhs.first < rhs.first;
              return lhs.second < rhs.second;
            });

  std::vector<std::string> out;
  out.reserve(scored.size());
  for (auto &[_, word] : scored) out.push_back(std::move(word));
  return out;
}

}  // namespace einheit::cli::fuzzy
