#include <gtest/gtest.h>
#include <CheckedTable.h>
#include "Epub/TocPageSelection.h"
#include <algorithm>
#include <limits>

TEST(CheckedTable, PsramLargeTableSortAndResizePreserveValues) {
  HalMemory::present = true;
  HalMemory::fail = false;
  CheckedTable<uint32_t> table;
  ASSERT_TRUE(table.reserve(7000));
  const auto allocations = HalMemory::allocations;
  for (unsigned i = 7000; i; --i) ASSERT_TRUE(table.push_back(i));
  EXPECT_EQ(HalMemory::allocations, allocations);
  std::sort(table.begin(), table.end());
  for (unsigned i = 0; i < 7000; ++i) EXPECT_EQ(table[i], i + 1);
  ASSERT_TRUE(table.resize(7500, 42));
  EXPECT_EQ(table[6999], 7000u);
  EXPECT_EQ(table[7499], 42u);
  table.clear();
  EXPECT_TRUE(table.empty());
}

TEST(CheckedTable, FailedPsramGrowthPreservesContentsWithoutInternalFallback) {
  HalMemory::present = true;
  HalMemory::fail = false;
  CheckedTable<uint32_t> table;
  ASSERT_TRUE(table.resize(8, 17));
  HalMemory::fail = true;
  EXPECT_FALSE(table.resize(7000));
  EXPECT_EQ(table.size(), 8u);
  for (const auto value : table) EXPECT_EQ(value, 17u);
  EXPECT_FALSE(table.reserve(std::numeric_limits<size_t>::max()));
  HalMemory::fail = false;
}

TEST(CheckedTable, AbsentPsramRetainsSmallInternalBlocksAndSorting) {
  HalMemory::present = false;
  HalMemory::fail = true;
  const auto allocations = HalMemory::allocations;
  CheckedTable<uint32_t> table;
  for (unsigned i = 7000; i; --i) ASSERT_TRUE(table.push_back(i));
  std::sort(table.begin(), table.end());
  for (unsigned i = 0; i < 7000; ++i) EXPECT_EQ(table[i], i + 1);
  EXPECT_EQ(HalMemory::allocations, allocations);
  HalMemory::present = true;
  HalMemory::fail = false;
}

TEST(TocPageSelection, LatestAnchorAtOrBeforePageWinsIncludingReversePaging) {
  for (int page = 0; page < 30; ++page) {
    TocPageSelection selection(10, page);
    selection.consider(10, 0);
    selection.consider(11, 8);
    selection.consider(12, 20);
    EXPECT_EQ(selection.selected(), page < 8 ? 10 : page < 20 ? 11 : 12);
  }
  TocPageSelection previous(10, 7);
  previous.consider(12, 20);
  previous.consider(11, 8);
  previous.consider(10, 0);
  EXPECT_EQ(previous.selected(), 10);
}

TEST(TocPageSelection, UnknownFutureAndEqualPageAnchors) {
  TocPageSelection selection(2, 9);
  selection.consider(3, std::nullopt);
  selection.consider(4, 10);
  EXPECT_EQ(selection.selected(), 2);
  selection.consider(5, 8);
  selection.consider(6, 8);
  selection.consider(7, 3);
  EXPECT_EQ(selection.selected(), 6);
  TocPageSelection empty(-1, 0);
  EXPECT_EQ(empty.selected(), -1);
}
