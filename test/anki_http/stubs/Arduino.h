#pragma once
#include <cstdint>
#include <string>

using String = std::string;
namespace test_clock {
inline uint32_t nowMs = 0;
}
inline uint32_t millis() { return test_clock::nowMs; }
inline void delay(uint32_t ms) { test_clock::nowMs += ms; }
struct TestEsp {
  unsigned getFreeHeap() const { return 1024 * 1024; }
  unsigned getMaxAllocHeap() const { return 1024 * 1024; }
};
inline TestEsp ESP;
