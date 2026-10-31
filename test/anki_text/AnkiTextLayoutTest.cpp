#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "anki/AnkiTextLayout.h"

namespace {
using namespace anki::text;

int width(void*, const char* value, const size_t size, const bool bold) {
  int result = 0;
  for (size_t i = 0; i < size; ++i) {
    if ((static_cast<unsigned char>(value[i]) & 0xc0) != 0x80) result += bold ? 2 : 1;
  }
  return result;
}

struct CapturedRun {
  std::string text;
  bool bold;
};

void collect(void* context, const char* value, const size_t size, const bool bold) {
  static_cast<std::vector<CapturedRun>*>(context)->push_back({std::string(value, size), bold});
}

TEST(AnkiTextLayout, StylesInsideAWordDoNotInsertSpaces) {
  const char text[] =
      "co\x02"
      "lo\x03"
      "ur";
  Line lines[4];
  const auto page = layoutPage(text, strlen(text), {}, 20, lines, 4, {nullptr, width});
  ASSERT_EQ(page.lineCount, 1u);
  std::vector<CapturedRun> runs;
  visitRuns(text, lines[0], {&runs, collect});
  ASSERT_EQ(runs.size(), 3u);
  EXPECT_EQ(runs[0].text, "co");
  EXPECT_FALSE(runs[0].bold);
  EXPECT_EQ(runs[1].text, "lo");
  EXPECT_TRUE(runs[1].bold);
  EXPECT_EQ(runs[2].text, "ur");
  EXPECT_FALSE(runs[2].bold);
}

TEST(AnkiTextLayout, WrapsAtAWordBoundary) {
  const char text[] = "one two three";
  Line lines[4];
  const auto page = layoutPage(text, strlen(text), {}, 7, lines, 4, {nullptr, width});
  ASSERT_EQ(page.lineCount, 2u);
  EXPECT_EQ(std::string(text + lines[0].start.offset, lines[0].end - lines[0].start.offset), "one two");
  EXPECT_EQ(std::string(text + lines[1].start.offset, lines[1].end - lines[1].start.offset), "three");
  EXPECT_FALSE(page.hasMore);
}

TEST(AnkiTextLayout, PreservesBoldAcrossPageBoundaries) {
  const char text[] =
      "\x02"
      "abcdef\x03"
      "g";
  Line lines[1];
  auto page = layoutPage(text, strlen(text), {}, 4, lines, 1, {nullptr, width});
  ASSERT_TRUE(page.hasMore);
  EXPECT_EQ(page.next.offset, 3u);
  EXPECT_TRUE(page.next.bold);
  page = layoutPage(text, strlen(text), page.next, 4, lines, 1, {nullptr, width});
  EXPECT_EQ(page.next.offset, 5u);
  EXPECT_TRUE(page.next.bold);
  page = layoutPage(text, strlen(text), page.next, 4, lines, 1, {nullptr, width});
  EXPECT_TRUE(page.hasMore);
  EXPECT_FALSE(page.next.bold);
  page = layoutPage(text, strlen(text), page.next, 4, lines, 1, {nullptr, width});
  EXPECT_FALSE(page.hasMore);
}

TEST(AnkiTextLayout, HardWrapNeverSplitsUtf8) {
  const char text[] = "日本語é";
  Line lines[4];
  const auto page = layoutPage(text, strlen(text), {}, 1, lines, 4, {nullptr, width});
  ASSERT_EQ(page.lineCount, 4u);
  EXPECT_EQ(lines[0].end, 3u);
  EXPECT_EQ(lines[1].end, 6u);
  EXPECT_EQ(lines[2].end, 9u);
  EXPECT_EQ(lines[3].end, 11u);
  EXPECT_FALSE(page.hasMore);
}

TEST(AnkiTextLayout, PreservesBlankLinesAndMultipleSpaces) {
  const char text[] = "a  b\n\nc";
  Line lines[4];
  const auto page = layoutPage(text, strlen(text), {}, 20, lines, 4, {nullptr, width});
  ASSERT_EQ(page.lineCount, 3u);
  EXPECT_EQ(lines[0].end, 4u);
  EXPECT_EQ(lines[1].start.offset, lines[1].end);
}

TEST(AnkiTextLayout, OversizedGlyphStillMakesProgress) {
  const char text[] =
      "\x02"
      "xy";
  Line lines[1];
  const auto page = layoutPage(text, strlen(text), {}, 1, lines, 1, {nullptr, width});
  EXPECT_EQ(page.next.offset, 2u);
  EXPECT_TRUE(page.hasMore);
}

TEST(AnkiTextLayout, RepeatedAndUnclosedMarkersRemainZeroWidth) {
  const char text[] =
      "\x03\x03\x02\x02"
      "a";
  Line lines[1];
  const auto page = layoutPage(text, strlen(text), {}, 2, lines, 1, {nullptr, width});
  EXPECT_FALSE(page.hasMore);
  std::vector<CapturedRun> runs;
  visitRuns(text, lines[0], {&runs, collect});
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].text, "a");
  EXPECT_TRUE(runs[0].bold);
}

TEST(AnkiTextLayout, EmptyInputAndZeroCapacityAreBounded) {
  Line line;
  EXPECT_EQ(layoutPage("", 0, {}, 0, &line, 1, {nullptr, width}).lineCount, 1u);
  EXPECT_TRUE(layoutPage("a", 1, {}, 1, nullptr, 0, {nullptr, width}).hasMore);
}

TEST(AnkiTextLayout, FlattensDesktopBlocksAndRetainsBold) {
  char text[] =
      "\x04"
      "table c=2 r=1 f=H\n\x02"
      "head\x03\x06"
      "value\n\x04"
      "/table\nend";
  normalizePreparedText(text);
  EXPECT_STREQ(text,
               "\x02"
               "head\x03|value\nend");
}

TEST(AnkiTextLayout, NormalizesWhitespaceAndUnknownControlsInPlace) {
  char text[] = "a\r\nb\rc\td\x01\x05";
  normalizePreparedText(text);
  EXPECT_STREQ(text, "a\nb\nc d");
}
TEST(AnkiTextLayout, EntireMaximumSideCanBePagedWithoutDroppingUtf8) {
  std::string text;
  for (int i = 0; i < 4000; ++i) text += "é";
  text += std::string(8000, 'x');
  Cursor cursor;
  Line lines[3];
  std::vector<CapturedRun> runs;
  size_t pages = 0;
  do {
    const auto page = layoutPage(text.data(), text.size(), cursor, 31, lines, 3, {nullptr, width});
    ASSERT_GT(page.next.offset, cursor.offset);
    for (size_t i = 0; i < page.lineCount; ++i) visitRuns(text.data(), lines[i], {&runs, collect});
    cursor = page.next;
    ASSERT_LT(++pages, 200u);
    if (!page.hasMore) break;
  } while (true);
  std::string displayed;
  for (const auto& run : runs) displayed += run.text;
  EXPECT_EQ(displayed, text);
  EXPECT_EQ(cursor.offset, text.size());
}

TEST(AnkiTextLayout, WordBoundaryRewindsStyleBeforeReprocessingMarkers) {
  const char text[] =
      "a "
      "\x02"
      "bcdefgh"
      "\x03";
  Line lines[1];
  auto page = layoutPage(text, strlen(text), {}, 5, lines, 1, {nullptr, width});
  ASSERT_TRUE(page.hasMore);
  EXPECT_EQ(page.next.offset, 2u);
  EXPECT_FALSE(page.next.bold);
  page = layoutPage(text, strlen(text), page.next, 5, lines, 1, {nullptr, width});
  std::vector<CapturedRun> runs;
  visitRuns(text, lines[0], {&runs, collect});
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].text, "bc");
  EXPECT_TRUE(runs[0].bold);
  EXPECT_TRUE(page.next.bold);
}
}  // namespace
