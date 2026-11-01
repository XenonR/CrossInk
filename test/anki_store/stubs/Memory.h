#pragma once

#define makeUniqueNoThrow makeUniqueNoThrowForAnkiTests
#include "../../../lib/Memory/Memory.h"
#undef makeUniqueNoThrow

// Test-only counterpart of lib/Memory's helpers. Firmware keeps its ordinary
// allocator; native tests can fail the review schedule without global new hooks.
namespace anki_test_memory {
inline bool failNextArray = false;
inline size_t arrayAllocationCalls = 0;
inline size_t lastArrayBytes = 0;
inline void reset() {
  failNextArray = false;
  arrayAllocationCalls = 0;
  lastArrayBytes = 0;
}
}  // namespace anki_test_memory

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return makeUniqueNoThrowForAnkiTests<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = std::remove_extent_t<T>;
  ++anki_test_memory::arrayAllocationCalls;
  anki_test_memory::lastArrayBytes = sizeof(Elem) * count;
  if (std::exchange(anki_test_memory::failNextArray, false)) return {};
  return makeUniqueNoThrowForAnkiTests<T>(count);
}
