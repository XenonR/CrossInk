#pragma once
#include <Arduino.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

constexpr int O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 4, O_TRUNC = 8, O_APPEND = 16;
class HalFile;
struct TestStorage {
  using Files = std::map<std::string, std::vector<uint8_t>>;
  Files files;
  size_t bytesRead = 0;
  size_t openCalls = 0;
  bool captureCheckpoints = false;
  std::vector<Files> checkpoints;
  std::map<std::string, int> handles;
  int failAfter = -1;
  bool failWrite = false, failSync = false, failClose = false;
  bool failRename = false;
  std::string failSyncPath;
  bool operation() {
    if (captureCheckpoints) checkpoints.push_back(files);
    if (failAfter < 0) return true;
    if (failAfter-- == 0) {
      failAfter = -1;
      return false;
    }
    return true;
  }
  bool mkdir(const char*) { return operation(); }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) {
    if (!operation() || handles[path]) return false;
    return files.erase(path) > 0;
  }
  bool rename(const char* from, const char* to) {
    if (!operation() || std::exchange(failRename, false) || !exists(from) || exists(to) || handles[from] || handles[to])
      return false;
    files[to] = std::move(files[from]);
    files.erase(from);
    return true;
  }
  HalFile open(const char* path, int flags = O_RDONLY);
  bool openFileForRead(const char*, const char* path, HalFile& file);
  bool writeFile(const char* path, const String& content);
  String readFile(const char* path);
};
inline TestStorage Storage;
class HalFile {
  std::string path_;
  size_t pos_ = 0;
  int flags_ = 0;

 public:
  HalFile() = default;
  HalFile(std::string path, int flags) : path_(std::move(path)), flags_(flags) {
    ++Storage.handles[path_];
    if (flags & O_APPEND) pos_ = size();
  }
  ~HalFile() {
    if (*this) close();
  }
  HalFile(HalFile&& other) noexcept : path_(std::exchange(other.path_, {})), pos_(other.pos_), flags_(other.flags_) {}
  HalFile& operator=(HalFile&& other) noexcept {
    if (*this) close();
    path_ = std::exchange(other.path_, {});
    pos_ = other.pos_;
    flags_ = other.flags_;
    return *this;
  }
  explicit operator bool() const { return !path_.empty(); }
  size_t size() const { return *this ? Storage.files.at(path_).size() : 0; }
  size_t fileSize() const { return size(); }
  size_t position() const { return pos_; }
  bool seek(size_t n) {
    if (!*this || !Storage.operation()) return false;
    pos_ = n;
    return true;
  }
  int read(void* out, size_t n) {
    if (!*this || !Storage.operation() || pos_ > size()) return -1;
    n = std::min(n, size() - pos_);
    Storage.bytesRead += n;
    if (n) memcpy(out, Storage.files[path_].data() + pos_, n);
    pos_ += n;
    return n;
  }
  size_t write(const void* data, size_t n) {
    if (!*this || !Storage.operation()) return 0;
    if (Storage.failWrite) {
      Storage.failWrite = false;
      if (n) --n;
    }
    auto& bytes = Storage.files[path_];
    if (pos_ + n > bytes.size()) bytes.resize(pos_ + n);
    if (n) memcpy(bytes.data() + pos_, data, n);
    pos_ += n;
    return n;
  }
  bool sync() {
    if (Storage.failSyncPath == path_) {
      Storage.failSyncPath.clear();
      return false;
    }
    if (std::exchange(Storage.failSync, false)) return false;
    return Storage.operation();
  }
  bool close() {
    if (!*this) return false;
    --Storage.handles[path_];
    path_.clear();
    if (std::exchange(Storage.failClose, false)) return false;
    return Storage.operation();
  }
};
inline HalFile TestStorage::open(const char* path, int flags) {
  ++openCalls;
  if (!operation() || handles[path]) return {};
  if (!exists(path)) {
    if (!(flags & O_CREAT)) return {};
    files[path] = {};
  }
  if (flags & O_TRUNC) files[path].clear();
  return HalFile(path, flags);
}
inline bool TestStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  file = open(path, O_RDONLY);
  return static_cast<bool>(file);
}
inline bool TestStorage::writeFile(const char* path, const String& content) {
  auto file = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return false;
  const bool written = file.write(content.c_str(), content.size()) == content.size();
  const bool synced = written && file.sync();
  const bool closed = file.close();
  return synced && closed;
}
inline String TestStorage::readFile(const char* path) {
  auto file = open(path, O_RDONLY);
  if (!file) return {};
  String content;
  content.resize(file.size());
  const bool read = file.read(content.data(), content.size()) == static_cast<int>(content.size());
  const bool closed = file.close();
  return read && closed ? content : String();
}
using FsFile = HalFile;
