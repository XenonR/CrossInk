#pragma once
#include <Arduino.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
#define ESP_ERR_HTTP_EAGAIN 0x7007
inline const char* esp_err_to_name(esp_err_t) { return "stub HTTP failure"; }
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_POST };
struct TestHttpClient;
using esp_http_client_handle_t = TestHttpClient*;
enum http_event { HTTP_EVENT_ON_HEADER };
struct esp_http_client_event_t {
  http_event event_id{};
  void* user_data = nullptr;
  const char* header_key = nullptr;
  const char* header_value = nullptr;
};
struct esp_http_client_config_t {
  const char* url = nullptr;
  esp_http_client_method_t method = HTTP_METHOD_GET;
  int buffer_size = 0, buffer_size_tx = 0, timeout_ms = 0;
  esp_err_t (*crt_bundle_attach)(void*) = nullptr;
  bool keep_alive_enable = false, disable_auto_redirect = false;
  esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
  void* user_data = nullptr;
};

namespace test_http {
struct Scenario {
  int status = 200;
  std::string response = "{}";
  int64_t declaredLength = 2;
  bool complete = true, failInit = false, failOpen = false, failWrite = false, failRead = false;
  bool failTimeout = false;
  std::string failHeader;
  size_t writeChunk = 4096, readChunk = 2048;
  std::deque<int> readResults;
};
struct Trace {
  esp_http_client_config_t config;
  std::string url, body;
  std::map<std::string, std::string> headers;
  int initCalls = 0, openCalls = 0, writeCalls = 0, readCalls = 0, cleanupCalls = 0;
  int openedLength = -1;
  int timeoutCalls = 0, readTimeoutMs = 0, openTimeoutMs = 0, headerTimeoutMs = 0;
};
inline Scenario scenario;
inline Trace trace;
inline void reset() {
  scenario = {};
  trace = {};
  test_clock::nowMs = 0;
}
}  // namespace test_http

struct TestHttpClient {
  size_t offset = 0;
  int timeoutMs = 0;
};
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
  ++test_http::trace.initCalls;
  test_http::trace.config = *config;
  test_http::trace.url = config->url;
  return test_http::scenario.failInit ? nullptr : new TestHttpClient{0, config->timeout_ms};
}
inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char* name, const char* value) {
  if (test_http::scenario.failHeader == name) return ESP_FAIL;
  test_http::trace.headers[name] = value;
  return ESP_OK;
}
inline esp_err_t esp_http_client_open(esp_http_client_handle_t client, const int length) {
  ++test_http::trace.openCalls;
  test_http::trace.openedLength = length;
  test_http::trace.openTimeoutMs = client->timeoutMs;
  return test_http::scenario.failOpen ? ESP_FAIL : ESP_OK;
}
inline int esp_http_client_write(esp_http_client_handle_t, const char* bytes, int length) {
  ++test_http::trace.writeCalls;
  if (test_http::scenario.failWrite) return -1;
  const size_t accepted = std::min(static_cast<size_t>(length), test_http::scenario.writeChunk);
  test_http::trace.body.append(bytes, accepted);
  return static_cast<int>(accepted);
}
inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) {
  test_http::trace.headerTimeoutMs = client->timeoutMs;
  return test_http::scenario.declaredLength;
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t) { return test_http::scenario.status; }
inline esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeoutMs) {
  ++test_http::trace.timeoutCalls;
  test_http::trace.readTimeoutMs = timeoutMs;
  if (test_http::scenario.failTimeout) return ESP_FAIL;
  client->timeoutMs = timeoutMs;
  return ESP_OK;
}
inline int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int capacity) {
  ++test_http::trace.readCalls;
  if (test_http::scenario.failRead) return -1;
  if (!test_http::scenario.readResults.empty()) {
    const int result = test_http::scenario.readResults.front();
    test_http::scenario.readResults.pop_front();
    if (result == -ESP_ERR_HTTP_EAGAIN) test_clock::nowMs += client->timeoutMs;
    if (result <= 0) return result;
    capacity = std::min(capacity, result);
  }
  const auto& response = test_http::scenario.response;
  const size_t count =
      std::min({response.size() - client->offset, static_cast<size_t>(capacity), test_http::scenario.readChunk});
  memcpy(buffer, response.data() + client->offset, count);
  client->offset += count;
  return static_cast<int>(count);
}
inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
  return test_http::scenario.complete && client->offset == test_http::scenario.response.size();
}
inline esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, int* error, int* flags) {
  *error = *flags = 0;
  return ESP_OK;
}
inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
  ++test_http::trace.cleanupCalls;
  delete client;
  return ESP_OK;
}
