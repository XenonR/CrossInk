#pragma once

#include <cstddef>
#include <cstdint>

namespace anki::text {

struct Cursor {
  size_t offset = 0;
  bool bold = false;
};

struct Line {
  Cursor start;
  size_t end = 0;
};

struct Page {
  Cursor next;
  size_t lineCount = 0;
  bool hasMore = false;
};

// Borrowed UTF-8 spans are not NUL-terminated. Measurement and drawing must use
// the same font metrics. No allocations or retained pointers are made here.
struct Measurer {
  void* context = nullptr;
  int (*width)(void*, const char*, size_t, bool) = nullptr;
};

struct RunSink {
  void* context = nullptr;
  void (*run)(void*, const char*, size_t, bool) = nullptr;
};

Page layoutPage(const char* text, size_t length, Cursor start, int maxWidth, Line* lines, size_t capacity,
                Measurer measure);
void visitRuns(const char* text, const Line& line, RunSink sink);

// Flatten the plugin's table/figure directives while retaining the prepared
// text and STX/ETX bold markers. Operates in place and never grows the buffer.
void normalizePreparedText(char* text);

}  // namespace anki::text
