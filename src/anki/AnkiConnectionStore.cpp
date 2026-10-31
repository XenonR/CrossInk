#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_ANKI
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <PersistableStore.h>

#include "AnkiConnectionStore.h"
#include "network/HttpRedirectPolicy.h"

namespace anki {
namespace {
constexpr char PATH[] = "/.crossink-anki/connection.json";
constexpr size_t MAX_CONFIG_BYTES = 2048;
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
  const char* readPath = Storage.exists(PATH) ? PATH : "/.crossink-anki/connection.json.bak";
  if (!Storage.exists(readPath)) return true;
  FsFile file;
  if (!Storage.openFileForRead("ANKI", readPath, file)) {
    LOG_ERR("ANKI", "Cannot read connection settings");
    return false;
  }
  const bool bounded = file.fileSize() <= MAX_CONFIG_BYTES;
  const bool closed = file.close();
  if (!bounded || !closed) {
    LOG_ERR("ANKI", "Connection settings exceed bounds or cannot close");
    return false;
  }
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(PATH, doc) || doc["version"].as<int>() != 1) {
    LOG_ERR("ANKI", "Invalid connection settings");
    return false;
  }
  Connection candidate;
  candidate.url = doc["url"] | "";
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  candidate.token = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", MAX_TOKEN_BYTES, &status);
  if ((status != obfuscation::DecodeStatus::VALIDATED && status != obfuscation::DecodeStatus::EMPTY) ||
      !normalizeUrl(candidate.url) || !validToken(candidate.token)) {
    LOG_ERR("ANKI", "Unreadable connection credentials");
    return false;
  }
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
