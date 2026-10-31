#pragma once
#include <cstdint>
#include <string>

using String = std::string;
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
struct TestEsp {
  unsigned getFreeHeap() const { return 1024 * 1024; }
  unsigned getMaxAllocHeap() const { return 1024 * 1024; }
};
inline TestEsp ESP;
