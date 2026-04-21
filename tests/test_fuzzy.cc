/// @file test_fuzzy.cc
/// @brief Tests for Levenshtein distance and Suggest().
// Copyright (c) 2026 Einheit Networks

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/fuzzy.h"

namespace einheit::cli::fuzzy {

TEST(Fuzzy, DistanceIdentical) {
  EXPECT_EQ(Distance("show", "show"), 0u);
}

TEST(Fuzzy, DistanceOneSubstitution) {
  EXPECT_EQ(Distance("show", "shaw"), 1u);
}

TEST(Fuzzy, DistanceInsertion) {
  EXPECT_EQ(Distance("show", "shows"), 1u);
}

TEST(Fuzzy, DistanceDeletion) {
  EXPECT_EQ(Distance("show", "sow"), 1u);
}

TEST(Fuzzy, DistanceCompletelyDifferent) {
  EXPECT_EQ(Distance("abc", "xyz"), 3u);
}

TEST(Fuzzy, SuggestsWithinThreshold) {
  const std::vector<std::string> vocab{
      "show", "set", "commit", "configure"};
  auto s = Suggest("shaw", vocab);
  ASSERT_FALSE(s.empty());
  EXPECT_EQ(s.front(), "show");
}

TEST(Fuzzy, NoSuggestionForFarStrings) {
  const std::vector<std::string> vocab{"show", "set"};
  auto s = Suggest("totallyunrelated", vocab);
  EXPECT_TRUE(s.empty());
}

TEST(Fuzzy, SuggestsHostnameTypo) {
  const std::vector<std::string> vocab{"hostname", "port", "mode"};
  auto s = Suggest("hostanme", vocab);
  ASSERT_FALSE(s.empty());
  EXPECT_EQ(s.front(), "hostname");
}

TEST(Fuzzy, PrefixMatchWinsOverEditDistance) {
  const std::vector<std::string> vocab{"show", "shows", "sho"};
  auto s = Suggest("show", vocab);
  // Both "shows" and "sho" are edit-distance 1. "shows" starts
  // with "show" (prefix score 0) so it ranks first; "sho" is not
  // a prefix (query longer than candidate) and ranks second.
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0], "shows");
  EXPECT_EQ(s[1], "sho");
}

TEST(Fuzzy, TypedPrefixPicksLongerWordPastThreshold) {
  // Classic case: user types "config", the canonical command is
  // "configure" — edit distance 3 but a clear prefix match.
  const std::vector<std::string> vocab{"configure", "commit",
                                        "rollback"};
  auto s = Suggest("config", vocab);
  ASSERT_FALSE(s.empty());
  EXPECT_EQ(s.front(), "configure");
}

}  // namespace einheit::cli::fuzzy
