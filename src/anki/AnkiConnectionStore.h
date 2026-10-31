#pragma once

#include <string>

namespace anki {
struct Connection {
  std::string url;
  std::string token;
  bool configured() const { return !url.empty() && !token.empty(); }
};

class AnkiConnectionStore {
 public:
  static constexpr size_t MAX_URL_BYTES = 192;
  static constexpr size_t MAX_TOKEN_BYTES = 128;
  static bool load(Connection& out);
  static bool save(const Connection& connection);
  // Only a server origin is accepted; endpoints are owned by the client.
  static bool normalizeUrl(std::string& url);
  static bool validToken(const std::string& token);
};
}  // namespace anki
