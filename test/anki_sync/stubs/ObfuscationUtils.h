#pragma once

#include <Arduino.h>

#include <cstring>

// Boundary fake: exercise the connection store and real atomic persistence,
// while the existing device-bound encoding remains owned by Serialization.
namespace obfuscation {
enum class DecodeStatus : uint8_t { EMPTY, INVALID, TOO_LONG, LEGACY, VALIDATED };
inline String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return {};
  return String("encoded:" + plaintext);
}
inline std::string deobfuscateFromBase64(const char* encoded, const size_t limit, DecodeStatus* status) {
  if (!*encoded) {
    *status = DecodeStatus::EMPTY;
    return {};
  }
  if (strncmp(encoded, "encoded:", 8)) {
    *status = DecodeStatus::INVALID;
    return {};
  }
  const std::string decoded = encoded + 8;
  *status = decoded.size() <= limit ? DecodeStatus::VALIDATED : DecodeStatus::TOO_LONG;
  return *status == DecodeStatus::VALIDATED ? decoded : std::string();
}
}  // namespace obfuscation
