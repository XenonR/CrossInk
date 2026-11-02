#pragma once

#define makeAlignedByteBufferNoThrow makeAlignedByteBufferNoThrowForAnkiProtocolTests
#include "../../../lib/Memory/Memory.h"
#undef makeAlignedByteBufferNoThrow

namespace anki_protocol_test_memory {
inline bool failNextArena = false;
inline size_t arenaAllocationCalls = 0;
inline size_t lastArenaBytes = 0;
inline void reset() {
  failNextArena = false;
  arenaAllocationCalls = lastArenaBytes = 0;
}
}  // namespace anki_protocol_test_memory

inline HeapByteBuffer makeAlignedByteBufferNoThrow(const size_t count, const MemoryPool pool = MemoryPool::None) {
  ++anki_protocol_test_memory::arenaAllocationCalls;
  anki_protocol_test_memory::lastArenaBytes = count;
  if (std::exchange(anki_protocol_test_memory::failNextArena, false)) return {};
  return makeAlignedByteBufferNoThrowForAnkiProtocolTests(count, pool);
}
