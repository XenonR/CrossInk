#pragma once
#include <cstddef>
#include <cstdint>

class Stream {
 public:
  virtual ~Stream() = default;
  virtual size_t write(const uint8_t*, size_t) = 0;
};
