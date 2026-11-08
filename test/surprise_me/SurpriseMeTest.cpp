#include <gtest/gtest.h>

#include "util/SurpriseMe.h"

namespace {

surprise::Candidate inbox(const char* path, int progress = -1, bool shortRead = true) {
  return surprise::Candidate{path, surprise::Origin::Inbox, progress, shortRead};
}

surprise::Candidate library(const char* path, int progress = -1) {
  return surprise::Candidate{path, surprise::Origin::Library, progress, false};
}

}  // namespace

TEST(SurpriseMe, EmptyAndIneligibleInputsPickNothing) {
  EXPECT_EQ(surprise::pick(nullptr, 0, 0), nullptr);
  const surprise::Candidate none[1] = {{nullptr, surprise::Origin::Inbox, -1, true}};
  EXPECT_EQ(surprise::pick(none, 1, 0), nullptr);
  const surprise::Candidate blank[1] = {inbox("")};
  EXPECT_EQ(surprise::pick(blank, 1, 0), nullptr);
  const surprise::Candidate finished[2] = {inbox("/books/Inbox/a.epub", 100), library("/books/b.epub", 100)};
  EXPECT_EQ(surprise::pick(finished, 2, 0), nullptr);
}

TEST(SurpriseMe, UnreadInboxBeatsEverything) {
  const surprise::Candidate items[4] = {
      library("/books/lib.epub"), inbox("/books/Inbox/reading.epub", 40), inbox("/books/Inbox/new.epub"),
      library("/books/started.epub", 12)};
  EXPECT_STREQ(surprise::pick(items, 4, 0), "/books/Inbox/new.epub");
}

TEST(SurpriseMe, ShortInProgressInboxBeatsUnreadLibrary) {
  const surprise::Candidate items[3] = {
      library("/books/lib.epub"), inbox("/books/Inbox/mid.epub", 35), library("/books/other.epub")};
  EXPECT_STREQ(surprise::pick(items, 3, 0), "/books/Inbox/mid.epub");
  // A long article already under way yields to a fresh book first.
  const surprise::Candidate longRead[2] = {inbox("/books/Inbox/long.epub", 35, false), library("/books/lib.epub")};
  EXPECT_STREQ(surprise::pick(longRead, 2, 0), "/books/lib.epub");
}

TEST(SurpriseMe, LibraryUnreadIsTheLastRealResort) {
  const surprise::Candidate items[3] = {library("/books/started.epub", 60), library("/books/new.epub"),
                                        library("/books/finished.epub", 100)};
  EXPECT_STREQ(surprise::pick(items, 3, 0), "/books/new.epub");
}

TEST(SurpriseMe, DeterministicChoiceWithinTheWinningBucket) {
  const surprise::Candidate items[4] = {inbox("/books/Inbox/a.epub"), library("/books/lib.epub"),
                                        inbox("/books/Inbox/b.epub"), inbox("/books/Inbox/c.epub")};
  EXPECT_STREQ(surprise::pick(items, 4, 0), "/books/Inbox/a.epub");
  EXPECT_STREQ(surprise::pick(items, 4, 1), "/books/Inbox/b.epub");
  EXPECT_STREQ(surprise::pick(items, 4, 2), "/books/Inbox/c.epub");
  EXPECT_STREQ(surprise::pick(items, 4, 3), "/books/Inbox/a.epub");  // roll wraps by bucket size
  EXPECT_STREQ(surprise::pick(items, 4, 1000), "/books/Inbox/b.epub");
}

TEST(SurpriseMe, RankBoundaries) {
  EXPECT_EQ(surprise::rank(inbox("/books/Inbox/a.epub", -1)), 0);
  EXPECT_EQ(surprise::rank(inbox("/books/Inbox/a.epub", 0)), 0);
  EXPECT_EQ(surprise::rank(inbox("/books/Inbox/a.epub", 99)), 1);
  EXPECT_EQ(surprise::rank(inbox("/books/Inbox/a.epub", 100)), -1);
  EXPECT_EQ(surprise::rank(library("/books/a.epub", -1)), 2);
  EXPECT_EQ(surprise::rank(library("/books/a.epub", 1)), 4);
  EXPECT_EQ(surprise::rank(library("")), -1);
}

TEST(SurpriseMe, StrongConceptMatchWinsItsBucket) {
  surprise::Candidate items[4] = {inbox("/books/Inbox/a.epub"), inbox("/books/Inbox/b.epub"),
                                  inbox("/books/Inbox/c.epub"), library("/books/lib.epub")};
  items[1].conceptDot = 9000;
  items[2].conceptDot = 12000;
  items[3].conceptDot = 16000;  // another bucket: never pulled forward
  for (uint32_t roll = 0; roll < 4; ++roll) EXPECT_STREQ(surprise::pick(items, 4, roll), "/books/Inbox/c.epub");
  // Weak matches leave the roll in charge.
  items[1].conceptDot = surprise::CONCEPT_PREFER_MIN_DOT - 1;
  items[2].conceptDot = 100;
  EXPECT_STREQ(surprise::pick(items, 4, 0), "/books/Inbox/a.epub");
  EXPECT_STREQ(surprise::pick(items, 4, 1), "/books/Inbox/b.epub");
  EXPECT_EQ(surprise::preferConceptMatch(items, 4, 0), nullptr);
  // Ties go to the first.
  items[0].conceptDot = surprise::CONCEPT_PREFER_MIN_DOT;
  items[1].conceptDot = surprise::CONCEPT_PREFER_MIN_DOT;
  EXPECT_STREQ(surprise::preferConceptMatch(items, 4, 0), "/books/Inbox/a.epub");
}
