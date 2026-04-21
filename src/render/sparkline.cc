/// @file sparkline.cc
/// @brief Sparkline rendering with ASCII fallback.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/sparkline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace einheit::cli::render {
namespace {

// U+2581..U+2588 as UTF-8 byte sequences. Eight height levels.
constexpr const char *kBlocks[] = {
    "\u2581", "\u2582", "\u2583", "\u2584",
    "\u2585", "\u2586", "\u2587", "\u2588",
};

}  // namespace

auto Sparkline(std::span<const double> samples,
               const TerminalCaps &caps) -> std::string {
  if (samples.empty()) return "";

  const auto [min_it, max_it] =
      std::minmax_element(samples.begin(), samples.end());
  const double min = *min_it;
  const double max = *max_it;

  if (!caps.unicode) {
    // Direction hint: compare first vs last, ignore intra-window.
    const double first = samples.front();
    const double last = samples.back();
    const double delta = last - first;
    const double range = max - min;
    if (range == 0.0 || std::abs(delta) < range * 0.05) return "flat";
    return delta > 0.0 ? "up" : "down";
  }

  if (max == min) {
    // All samples equal — render as the lowest block across the row.
    std::string out;
    for (std::size_t i = 0; i < samples.size(); ++i) out += kBlocks[0];
    return out;
  }

  std::string out;
  const double span = max - min;
  for (const double s : samples) {
    const double norm = (s - min) / span;
    int level = static_cast<int>(norm * 7.0 + 0.5);
    if (level < 0) level = 0;
    if (level > 7) level = 7;
    out += kBlocks[level];
  }
  return out;
}

}  // namespace einheit::cli::render
