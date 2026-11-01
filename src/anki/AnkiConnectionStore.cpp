#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_ANKI
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ObfuscationUtils.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstring>

#include "AnkiConnectionStore.h"
#include "network/HttpRedirectPolicy.h"

namespace anki {
namespace {
constexpr char PATH[] = "/.crossink-anki/connection.json";
constexpr char BACKUP_PATH[] = "/.crossink-anki/connection.json.bak";
constexpr size_t MAX_CONFIG_BYTES = 2048;

struct ConfigInput {
  const char* data;
  size_t size;
  size_t position = 0;
  int read() { return position < size ? static_cast<unsigned char>(data[position++]) : -1; }
  size_t readBytes(char* output, const size_t length) {
    const size_t count = std::min(length, size - position);
    memcpy(output, data + position, count);
    position += count;
    return count;
  }
};
}  // namespace

bool AnkiConnectionStore::normalizeUrl(std::string& url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (url.empty()) return true;
  if (url.size() > MAX_URL_BYTES || url.find_first_of(" @?#\\\"<>") != std::string::npos) return false;
  for (const unsigned char c : url) {
    if (c < 33 || c > 126) return false;
  }
  HttpRedirectPolicy::Url parsed;
  return HttpRedirectPolicy::parseUrl(url, parsed) && parsed.path == "/";
}

bool AnkiConnectionStore::validToken(const std::string& token) {
  if (token.size() > MAX_TOKEN_BYTES) return false;
  for (const unsigned char c : token) {
    if (c < 33 || c > 126) return false;
  }
  return true;
}

bool AnkiConnectionStore::load(Connection& out) {
  out = {};
  const char* readPath = Storage.exists(PATH) ? PATH : BACKUP_PATH;
  if (!Storage.exists(readPath)) return true;
  FsFile file;
  if (!Storage.openFileForRead("ANKI", readPath, file)) {
    LOG_ERR("ANKI", "Cannot read connection settings");
    return false;
  }
  const auto fileSize = file.fileSize();
  if (fileSize == 0 || fileSize > MAX_CONFIG_BYTES) {
    file.close();
    LOG_ERR("ANKI", "Connection settings exceed bounds or are empty");
    return false;
  }
  // At most 2 KiB of cold-path file input, bounded before allocation and kept
  // off the activity stack. Read once so parsing validates the checked file.
  const size_t size = static_cast<size_t>(fileSize);
  auto contents = makeUniqueNoThrow<char[]>(size);
  if (!contents) {
    file.close();
    LOG_ERR("ANKI", "Cannot allocate connection settings input");
    return false;
  }
  const bool read = file.read(contents.get(), size) == static_cast<int>(size);
  const bool closed = file.close();
  if (!read || !closed || memchr(contents.get(), '\0', size)) {
    LOG_ERR("ANKI", "Cannot read complete connection settings");
    return false;
  }
  JsonDocument doc;
  ConfigInput input{contents.get(), size};
  if (deserializeJson(doc, input, DeserializationOption::NestingLimit(4)) || !doc.is<JsonObject>() ||
      !doc["version"].is<unsigned>() || doc["version"].as<unsigned>() != 1 || !doc["url"].is<const char*>() ||
      !doc["token_obf"].is<const char*>()) {
    LOG_ERR("ANKI", "Invalid connection settings");
    return false;
  }
  while (input.position < size) {
    const char c = contents[input.position++];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      LOG_ERR("ANKI", "Trailing data in connection settings");
      return false;
    }
  }
  const JsonString url = doc["url"].as<JsonString>();
  const JsonString encodedToken = doc["token_obf"].as<JsonString>();
  // Preserve lengths until validation: C-string conversion would hide an
  // embedded NUL in a damaged address or credential payload.
  if (url.size() > MAX_URL_BYTES || memchr(url.c_str(), '\0', url.size()) ||
      memchr(encodedToken.c_str(), '\0', encodedToken.size())) {
    LOG_ERR("ANKI", "Invalid connection settings strings");
    return false;
  }
  Connection candidate;
  candidate.url.assign(url.c_str(), url.size());
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  candidate.token = obfuscation::deobfuscateFromBase64(encodedToken.c_str(), MAX_TOKEN_BYTES, &status);
  if ((status != obfuscation::DecodeStatus::VALIDATED && status != obfuscation::DecodeStatus::EMPTY) ||
      !normalizeUrl(candidate.url) || !validToken(candidate.token)) {
    LOG_ERR("ANKI", "Unreadable connection credentials");
    return false;
  }
  if (readPath == BACKUP_PATH && !Storage.rename(BACKUP_PATH, PATH))
    LOG_ERR("ANKI", "Could not restore connection settings backup; loaded directly");
  out = std::move(candidate);
  return true;
}

bool AnkiConnectionStore::save(const Connection& connection) {
  std::string normalized = connection.url;
  if (!normalizeUrl(normalized) || normalized != connection.url || !validToken(connection.token)) {
    LOG_ERR("ANKI", "Invalid connection settings");
    return false;
  }
  Storage.mkdir("/.crossink-anki");
  JsonDocument doc;
  doc["version"] = 1;
  doc["url"] = connection.url;
  const String encoded = obfuscation::obfuscateToBase64(connection.token);
  if (!connection.token.empty() && encoded.isEmpty()) {
    LOG_ERR("ANKI", "Cannot encode connection token");
    return false;
  }
  doc["token_obf"] = encoded;
  if (doc.overflowed()) {
    LOG_ERR("ANKI", "Cannot allocate connection settings document");
    return false;
  }
  return PersistableStoreBase::writeDocToFileAtomically(PATH, doc);
}
}  // namespace anki
#endif
