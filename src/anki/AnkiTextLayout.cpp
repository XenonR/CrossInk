#include "AnkiTextLayout.h"

#include <cstring>

namespace anki::text {
namespace {
constexpr unsigned char kBoldOn = 2;
constexpr unsigned char kBoldOff = 3;
constexpr unsigned char kBlock = 4;
constexpr unsigned char kCell = 6;

size_t codepointBytes(const char* text, const size_t remaining) {
  const auto first = static_cast<unsigned char>(*text);
  size_t bytes = first < 0x80 ? 1 : (first >= 0xc2 && first < 0xe0 ? 2 : (first < 0xf0 ? 3 : 4));
  if (first < 0xc2 || first > 0xf4 || bytes > remaining) return 1;
  for (size_t i = 1; i < bytes; ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) return 1;
  }
  return bytes;
}

bool styleMarker(const char byte) {
  return static_cast<unsigned char>(byte) == kBoldOn || static_cast<unsigned char>(byte) == kBoldOff;
}

Line nextLine(const char* text, const size_t length, Cursor& cursor, const int maxWidth, const Measurer measure) {
  Line line{cursor, cursor.offset};
  size_t offset = cursor.offset;
  size_t runStart = offset;
  int completedWidth = 0;
  bool bold = cursor.bold;
  bool hasText = false;
  Cursor lastBreak{};
  bool canBreak = false;

  while (offset < length) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    if (styleMarker(text[offset])) {
      if (offset > runStart) completedWidth += measure.width(measure.context, text + runStart, offset - runStart, bold);
      bold = byte == kBoldOn;
      runStart = ++offset;
      continue;
    }
    if (byte == '\n') {
      line.end = offset;
      cursor = {offset + 1, bold};
      return line;
    }
    const size_t end = offset + codepointBytes(text + offset, length - offset);
    const int width = completedWidth + measure.width(measure.context, text + runStart, end - runStart, bold);
    if (hasText && width > maxWidth) {
      if (byte == ' ' || byte == '\t') {
        line.end = offset;
        cursor = {end, bold};
        return line;
      }
      cursor = canBreak ? lastBreak : Cursor{offset, bold};
      line.end = cursor.offset;
      return line;
    }
    hasText = true;
    offset = end;
    if (byte == ' ' || byte == '\t') {
      lastBreak = {offset, bold};
      canBreak = true;
    }
  }
  line.end = length;
  cursor = {length, bold};
  return line;
}
}  // namespace

Page layoutPage(const char* text, const size_t length, Cursor start, const int maxWidth, Line* lines,
                const size_t capacity, const Measurer measure) {
  Page page;
  if (start.offset > length) start.offset = length;
  page.next = start;
  if (!text || !lines || capacity == 0 || !measure.width) {
    page.hasMore = start.offset < length;
    return page;
  }
  do {
    lines[page.lineCount++] = nextLine(text, length, page.next, maxWidth > 0 ? maxWidth : 1, measure);
  } while (page.next.offset < length && page.lineCount < capacity);
  page.hasMore = page.next.offset < length;
  return page;
}

void visitRuns(const char* text, const Line& line, const RunSink sink) {
  if (!text || !sink.run || line.end < line.start.offset) return;
  bool bold = line.start.bold;
  size_t start = line.start.offset;
  for (size_t offset = start; offset < line.end; ++offset) {
    if (!styleMarker(text[offset])) continue;
    if (offset > start) sink.run(sink.context, text + start, offset - start, bold);
    bold = static_cast<unsigned char>(text[offset]) == kBoldOn;
    start = offset + 1;
  }
  if (line.end > start) sink.run(sink.context, text + start, line.end - start, bold);
}

void normalizePreparedText(char* text) {
  if (!text) return;
  char* out = text;
  const char* in = text;
  while (*in) {
    if (static_cast<unsigned char>(*in) == kBlock) {
      while (*in && *in != '\n') ++in;
      if (*in == '\n') ++in;
      continue;
    }
    const auto byte = static_cast<unsigned char>(*in++);
    if (byte == kCell) {
      *out++ = '|';
    } else if (byte == '\r') {
      if (*in != '\n') *out++ = '\n';
    } else if (byte == '\t') {
      *out++ = ' ';
    } else if (byte >= ' ' || byte == '\n' || byte == kBoldOn || byte == kBoldOff) {
      *out++ = static_cast<char>(byte);
    }
  }
  *out = '\0';
}

}  // namespace anki::text
