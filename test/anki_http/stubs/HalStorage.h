#pragma once
#include <cstddef>
#include <cstdint>

constexpr int O_WRONLY = 1, O_APPEND = 2;
class FsFile {
 public:
  operator bool() const { return false; }
  size_t fileSize() const { return 0; }
  size_t write(const void*, size_t) { return 0; }
  bool sync() { return false; }
  bool close() { return false; }
};
struct TestStorage {
  bool exists(const char*) const { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  FsFile open(const char*, int) { return {}; }
  bool openFileForRead(const char*, const char*, FsFile&) { return false; }
  bool openFileForWrite(const char*, const char*, FsFile&) { return false; }
  uint64_t totalBytes() const { return 0; }
  uint64_t usedBytes() const { return 0; }
};
inline TestStorage Storage;
