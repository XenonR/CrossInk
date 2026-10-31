#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

class HttpDownloader {
 public:
  using DataCallback = std::function<bool(const uint8_t*, size_t)>;
  using CancelCallback = std::function<bool()>;
  enum DownloadError { OK, HTTP_ERROR, FILE_ERROR, ABORTED };
  static DownloadError requestToken(const std::string& url, const char* token, const char* method,
                                    std::string_view body, const DataCallback& onData, size_t maxResponseBytes,
                                    int& status, CancelCallback shouldCancel);
};
