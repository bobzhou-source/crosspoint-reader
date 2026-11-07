#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/activities/reader/Boomerang.h"
#include "src/activities/reader/ReadingPacing.h"

// Micro-checkpoints inside a long chapter, and the rule that picks the Inbox item
// for a Quick Break and recognises the Inbox itself. Both are pure decisions the
// readers share, so the arithmetic is pinned here instead of being verified by
// turning pages on a device.

namespace {

using reading_pacing::Checkpoint;
using reading_pacing::CheckpointKind;

std::string Countdown(const Checkpoint& checkpoint) {
  char buffer[32];
  const size_t written = reading_pacing::formatCountdown(checkpoint, buffer, sizeof(buffer));
  return written == 0 ? std::string() : std::string(buffer, written);
}

Checkpoint Next(const int currentPage, const int startPage, const int endPage,
                const std::vector<uint16_t>& markers = {}) {
  return reading_pacing::nextCheckpoint(currentPage, startPage, endPage, markers.data(), markers.size());
}

}  // namespace

TEST(ReadingPacingTest, ShortChaptersAreNotLongAndGetNoInterval) {
  EXPECT_FALSE(reading_pacing::isLongChapter(0, 5));   // six pages: one sitting
  EXPECT_TRUE(reading_pacing::isLongChapter(0, 6));    // seven pages: long
  EXPECT_TRUE(reading_pacing::isLongChapter(30, 69));  // a 40-page chapter
  EXPECT_FALSE(reading_pacing::isLongChapter(4, 3));   // empty span
}

TEST(ReadingPacingTest, ChapterWithoutMarkersUsesSixPageIntervals) {
  // A 40-page chapter, no headings and no rules: the first micro-goal is six
  // pages in, then every six pages.
  EXPECT_EQ(Next(0, 0, 39).page, 6);
  EXPECT_EQ(Next(0, 0, 39).kind, CheckpointKind::MicroBreak);
  EXPECT_EQ(Next(5, 0, 39).page, 6);
  EXPECT_EQ(Next(6, 0, 39).page, 12);
  EXPECT_EQ(Next(11, 0, 39).page, 12);
  EXPECT_EQ(Next(37, 0, 39).page, 39);  // interval would be 42, past the end
}

TEST(ReadingPacingTest, IntervalBoundariesCountFromTheChapterStart) {
  // A chapter that starts mid-book (a TXT heading at page 30) still gets its
  // micro-goals six pages in, not at absolute multiples of six.
  EXPECT_EQ(Next(30, 30, 45).page, 36);
  EXPECT_EQ(Next(35, 30, 45).page, 36);
  EXPECT_EQ(Next(36, 30, 45).page, 42);
}

TEST(ReadingPacingTest, ExplicitMarkersWinWhenTheyComeSooner) {
  // An <hr> / subheading four pages ahead beats the six-page interval.
  const std::vector<uint16_t> markers{4, 12};
  EXPECT_EQ(Next(0, 0, 39, markers).page, 4);
  EXPECT_EQ(Next(0, 0, 39, markers).kind, CheckpointKind::MicroBreak);
  // Standing on it, the next interval takes over before the far marker.
  EXPECT_EQ(Next(4, 0, 39, markers).page, 6);
  EXPECT_EQ(Next(11, 0, 39, markers).page, 12);
  // Past the last marker the intervals keep the cadence.
  EXPECT_EQ(Next(12, 0, 39, markers).page, 18);
}

TEST(ReadingPacingTest, MarkersBehindOrOnTheChapterEndAreIgnored) {
  // A marker at the chapter end is the chapter end, and one behind the reader is
  // already taken; neither may suppress the interval micro-goal.
  const std::vector<uint16_t> markers{0, 2, 39};
  EXPECT_EQ(Next(5, 0, 39, markers).page, 6);
  EXPECT_EQ(Next(6, 0, 39, markers).page, 12);
}

TEST(ReadingPacingTest, ChapterEndIsTheLastCheckpoint) {
  const Checkpoint checkpoint = Next(38, 0, 39);
  EXPECT_EQ(checkpoint.kind, CheckpointKind::ChapterEnd);
  EXPECT_EQ(checkpoint.page, 39);
  EXPECT_EQ(checkpoint.distance, 1);
  EXPECT_EQ(Countdown(checkpoint), "1p left");
}

TEST(ReadingPacingTest, CountdownAppearsOnlyInTheLastThreePages) {
  // A checkpoint four pages out is not a goal yet.
  EXPECT_FALSE(reading_pacing::nextCheckpoint(2, 0, 39, nullptr, 0).showsCountdown());
  EXPECT_EQ(Countdown(Next(2, 0, 39)), "");

  const std::vector<uint16_t> markers{10};
  EXPECT_EQ(Countdown(Next(7, 0, 39, markers)), "3p to break");
  EXPECT_EQ(Countdown(Next(8, 0, 39, markers)), "2p to break");
  EXPECT_EQ(Countdown(Next(9, 0, 39, markers)), "1p to break");
}

TEST(ReadingPacingTest, StandingOnACheckpointShowsNoCountdown) {
  EXPECT_EQ(Next(10, 0, 39, std::vector<uint16_t>{10}).page, 12);  // interval, not the marker itself
  const Checkpoint atEnd = Next(39, 0, 39);
  EXPECT_EQ(atEnd.kind, CheckpointKind::None);
  EXPECT_EQ(Countdown(atEnd), "");
  EXPECT_EQ(Countdown(Next(40, 0, 39)), "");
}

TEST(ReadingPacingTest, CountdownRespectsTheOutputBuffer) {
  char small[3];
  const Checkpoint checkpoint = Next(38, 0, 39);
  EXPECT_EQ(reading_pacing::formatCountdown(checkpoint, small, sizeof(small)), 0u);
  EXPECT_EQ(reading_pacing::formatCountdown(checkpoint, nullptr, 0), 0u);
  char exact[9];  // "1p left" plus the terminator
  EXPECT_EQ(reading_pacing::formatCountdown(checkpoint, exact, sizeof(exact)), 7u);
  EXPECT_STREQ(exact, "1p left");
}

TEST(BoomerangTest, InboxFolderIsRecognisedComponentwise) {
  EXPECT_TRUE(boomerang::isInboxBookPath("/books/Inbox/novel.epub"));
  EXPECT_TRUE(boomerang::isInboxBookPath("/books/Inbox"));
  EXPECT_TRUE(boomerang::isInboxBookPath("/BOOKS/inbox/novel.epub"));  // FAT is case-insensitive
  EXPECT_TRUE(boomerang::isInboxBookPath("/books/Inbox/sub/novel.epub"));
  EXPECT_TRUE(boomerang::isInboxBookPath("books/Inbox/novel.epub"));

  EXPECT_FALSE(boomerang::isInboxBookPath("/books/Inbox-old/novel.epub"));
  EXPECT_FALSE(boomerang::isInboxBookPath("/bookshelf/Inbox/novel.epub"));
  EXPECT_FALSE(boomerang::isInboxBookPath("/books/novel.epub"));
  EXPECT_FALSE(boomerang::isInboxBookPath("/books/Inbo/novel.epub"));
  EXPECT_FALSE(boomerang::isInboxBookPath(""));
}

TEST(BoomerangTest, UnreadItemIsPreferredOverStartedOne) {
  EXPECT_TRUE(boomerang::prefersBreakCandidate("", "/books/Inbox/a.epub", false));
  EXPECT_FALSE(boomerang::prefersBreakCandidate("", "/books/Inbox/a.epub", true));
  EXPECT_TRUE(boomerang::prefersBreakCandidate("/books/Inbox/b.epub", "/books/Inbox/a.epub", false));
  EXPECT_FALSE(boomerang::prefersBreakCandidate("/books/Inbox/a.epub", "/books/Inbox/b.epub", false));
  // A started candidate never displaces an unread one, however it sorts.
  EXPECT_FALSE(boomerang::prefersBreakCandidate("/books/Inbox/a.epub", "/books/Inbox/0.epub", true));
}

TEST(BoomerangTest, SelectionIsStableAndSkipsEverythingStarted) {
  const boomerang::Candidate candidates[] = {
      {"/books/Inbox/m.epub", true},
      {"/books/Inbox/z.epub", false},
      {"/books/Inbox/A.epub", false},
      {"/books/Inbox/b.epub", false},
  };
  // Case-insensitive order: A.epub, b.epub, z.epub -- the started m.epub is skipped.
  EXPECT_EQ(boomerang::selectBreakCandidate(candidates, 4), 2u);

  const boomerang::Candidate allStarted[] = {{"/books/Inbox/a.epub", true}};
  EXPECT_EQ(boomerang::selectBreakCandidate(allStarted, 1), 1u);
  EXPECT_EQ(boomerang::selectBreakCandidate(nullptr, 0), 0u);
}

TEST(BoomerangTest, DisplayNameDropsFolderAndExtension) {
  EXPECT_EQ(boomerang::displayName("/books/Inbox/short read.epub"), "short read");
  EXPECT_EQ(boomerang::displayName("novel.epub"), "novel");
  EXPECT_EQ(boomerang::displayName("/books/Inbox/no-extension"), "no-extension");
  EXPECT_EQ(boomerang::displayName("/books/Inbox/.hidden"), ".hidden");
}

TEST(BoomerangTest, BreakRowPointsOutOnlyWithAnUnreadItem) {
  using boomerang::BreakRowKind;
  constexpr std::string_view source = "/books/sapiens.epub";
  constexpr std::string_view inboxItem = "/books/Inbox/essay.epub";

  // No break open, an unread item waiting, the book is not the Inbox: the row is
  // the way out.
  EXPECT_EQ(boomerang::planBreakRow(source, "", false, true), BreakRowKind::QuickBreak);
  // Nothing waiting: no row at all.
  EXPECT_EQ(boomerang::planBreakRow(source, "", false, false), BreakRowKind::None);
  // Never offer a break from the Inbox itself.
  EXPECT_EQ(boomerang::planBreakRow(inboxItem, "", true, true), BreakRowKind::None);
  EXPECT_EQ(boomerang::planBreakRow("", "", false, true), BreakRowKind::None);
}

TEST(BoomerangTest, OpenBreakOnlyOffersTheWayBack) {
  using boomerang::BreakRowKind;
  constexpr std::string_view source = "/books/sapiens.epub";
  constexpr std::string_view inboxItem = "/books/Inbox/essay.epub";

  // Inside the break item the row is the way back, and it never stacks another
  // break even though more unread items may exist.
  EXPECT_EQ(boomerang::planBreakRow(inboxItem, source, true, true), BreakRowKind::BackToBook);
  // Back on the source book (reopened by hand) the stale break must not offer a
  // row pointing at itself.
  EXPECT_EQ(boomerang::planBreakRow(source, source, false, true), BreakRowKind::None);
}

TEST(BoomerangTest, ReturnResolvesToLiveMissingOrSelfTarget) {
  using boomerang::ReturnPlan;
  constexpr std::string_view source = "/books/sapiens.epub";
  constexpr std::string_view inboxItem = "/books/Inbox/essay.epub";

  EXPECT_EQ(boomerang::planReturn(inboxItem, "", false), ReturnPlan::Nothing);
  EXPECT_EQ(boomerang::planReturn(inboxItem, source, true), ReturnPlan::BackToBook);
  // A deleted or unmounted source book is not navigated to.
  EXPECT_EQ(boomerang::planReturn(inboxItem, source, false), ReturnPlan::BookGone);
  // The source reopened by hand ends the break instead of reopening itself.
  EXPECT_EQ(boomerang::planReturn(source, source, true), ReturnPlan::SelfBook);
}

TEST(BoomerangTest, ReturnLabelFallsBackToTheFileName) {
  constexpr std::string_view source = "/books/Sapiens.epub";
  EXPECT_EQ(boomerang::returnLabel("/books/Inbox/a.epub", source, "Sapiens"), "Sapiens");
  EXPECT_EQ(boomerang::returnLabel("/books/Inbox/a.epub", source, ""), "Sapiens");
  // Nothing to go back to: no label, so no row.
  EXPECT_EQ(boomerang::returnLabel("/books/Inbox/a.epub", "", "Sapiens"), "");
  EXPECT_EQ(boomerang::returnLabel(source, source, "Sapiens"), "");
}
