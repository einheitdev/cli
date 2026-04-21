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

TEST(Fuzzy, RanksByDistanceAscending) {
  const std::vector<std::string> vocab{"show", "shows", "sho"};
  auto s = Suggest("show", vocab);
  // All three are within threshold; "shows" and "sho" both distance 1.
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0], "sho");    // alphabetical tie break
  EXPECT_EQ(s[1], "shows");
}

}  // namespace einheit::cli::fuzzy
