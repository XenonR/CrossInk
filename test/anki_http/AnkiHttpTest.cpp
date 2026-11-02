#include <gtest/gtest.h>

#include "esp_http_client.h"
#include "network/HttpDownloader.h"

namespace AppVersion {
const char* userAgent() { return "CrossInk/Anki HTTP test"; }
}  // namespace AppVersion

namespace {
using Error = HttpDownloader::DownloadError;
class AnkiHttp : public ::testing::Test {
 protected:
  void SetUp() override { test_http::reset(); }
  std::string received;
  int status = -1;

  Error request(const char* path = "/pull", const char* token = "test-token", const char* method = "GET",
                const std::string_view body = {}, const size_t limit = 64,
                HttpDownloader::CancelCallback cancelled = nullptr) {
    return HttpDownloader::requestToken(
        std::string("http://desktop:8765") + path, token, method, body,
        [&](const uint8_t* data, const size_t count) {
          received.append(reinterpret_cast<const char*>(data), count);
          return true;
        },
        limit, status, std::move(cancelled));
  }
  void expectOneRequest() {
    EXPECT_EQ(test_http::trace.initCalls, 1);
    EXPECT_EQ(test_http::trace.openCalls, 1);
    EXPECT_EQ(test_http::trace.cleanupCalls, 1);
    EXPECT_TRUE(test_http::trace.config.disable_auto_redirect);
    EXPECT_GT(test_http::trace.config.timeout_ms, 0);
    EXPECT_FALSE(test_http::trace.config.keep_alive_enable);
  }
};

TEST_F(AnkiHttp, HealthUsesUnauthenticatedJsonGet) {
  ASSERT_EQ(request("/health", ""), Error::OK);
  EXPECT_EQ(status, 200);
  EXPECT_EQ(received, "{}");
  EXPECT_EQ(test_http::trace.config.method, HTTP_METHOD_GET);
  EXPECT_EQ(test_http::trace.headers.count("Authorization"), 0u);
  EXPECT_EQ(test_http::trace.headers.at("Accept"), "application/json");
  EXPECT_EQ(test_http::trace.openedLength, 0);
  EXPECT_EQ(test_http::trace.writeCalls, 0);
  expectOneRequest();
}

TEST_F(AnkiHttp, PullUsesBearerAndNdjsonAccept) {
  ASSERT_EQ(request(), Error::OK);
  EXPECT_EQ(test_http::trace.headers.at("Authorization"), "Bearer test-token");
  EXPECT_EQ(test_http::trace.headers.at("Accept"), "application/x-ndjson");
  EXPECT_EQ(test_http::trace.headers.at("Connection"), "close");
  EXPECT_EQ(test_http::trace.headers.at("User-Agent"), AppVersion::userAgent());
  EXPECT_EQ(test_http::trace.headers.count("Content-Length"), 0u);
  expectOneRequest();
}

TEST_F(AnkiHttp, PushSendsExactBodyAndExplicitLengthAcrossShortWrites) {
  test_http::scenario.writeChunk = 3;
  const std::string body = R"({"reviews":[]})";
  ASSERT_EQ(request("/push", "test-token", "POST", body), Error::OK);
  EXPECT_EQ(test_http::trace.config.method, HTTP_METHOD_POST);
  EXPECT_EQ(test_http::trace.headers.at("Authorization"), "Bearer test-token");
  EXPECT_EQ(test_http::trace.headers.at("Accept"), "application/json");
  EXPECT_EQ(test_http::trace.headers.at("Content-Type"), "application/json");
  EXPECT_EQ(test_http::trace.headers.at("Content-Length"), std::to_string(body.size()));
  EXPECT_EQ(test_http::trace.openedLength, static_cast<int>(body.size()));
  EXPECT_EQ(test_http::trace.body, body);
  EXPECT_GT(test_http::trace.writeCalls, 1);
  expectOneRequest();
}

TEST_F(AnkiHttp, RedirectsAndErrorsAreNeverRetriedOrDelivered) {
  for (int code : {301, 302, 303, 307, 308, 401, 403, 500}) {
    test_http::reset();
    test_http::scenario.status = code;
    EXPECT_EQ(request("/push", "test-token", "POST", "{}"), Error::HTTP_ERROR) << code;
    EXPECT_EQ(status, code);
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(test_http::trace.readCalls, 0);
    expectOneRequest();
  }
}

TEST_F(AnkiHttp, LostOrTruncatedResponseIsNotSuccessOrRetried) {
  test_http::scenario.response = "{";
  EXPECT_EQ(request("/push", "test-token", "POST", "{}"), Error::HTTP_ERROR);
  EXPECT_EQ(received, "{");
  expectOneRequest();
  test_http::reset();
  test_http::scenario.complete = false;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  expectOneRequest();
}

TEST_F(AnkiHttp, DeclaredOversizeIsRejectedBeforeDelivery) {
  test_http::scenario.declaredLength = 65;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  EXPECT_TRUE(received.empty());
  EXPECT_EQ(test_http::trace.readCalls, 0);
  expectOneRequest();
}

TEST_F(AnkiHttp, UnknownLengthIsStillBoundedBeforeCallback) {
  test_http::scenario.declaredLength = 0;
  test_http::scenario.response = "123456789";
  test_http::scenario.readChunk = 4;
  EXPECT_EQ(request("/pull", "test-token", "GET", {}, 6), Error::FILE_ERROR);
  EXPECT_EQ(received, "1234");
  expectOneRequest();
}

TEST_F(AnkiHttp, ExactLimitAndCompleteUnknownLengthAreAccepted) {
  test_http::scenario.declaredLength = 0;
  test_http::scenario.response = "12345678";
  test_http::scenario.readChunk = 3;
  EXPECT_EQ(request("/pull", "test-token", "GET", {}, 8), Error::OK);
  EXPECT_EQ(received, "12345678");
  expectOneRequest();
}

TEST_F(AnkiHttp, NegativeHeaderOrReadFailureIsNotRetried) {
  test_http::scenario.declaredLength = -1;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  expectOneRequest();
  test_http::reset();
  test_http::scenario.failRead = true;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  expectOneRequest();
}

TEST_F(AnkiHttp, ReadPollingPreservesTheConnectionAndHeaderTimeout) {
  ASSERT_EQ(request(), Error::OK);
  EXPECT_EQ(test_http::trace.config.timeout_ms, 75000);
  EXPECT_EQ(test_http::trace.openTimeoutMs, 75000);
  EXPECT_EQ(test_http::trace.headerTimeoutMs, 75000);
  EXPECT_EQ(test_http::trace.timeoutCalls, 1);
  EXPECT_EQ(test_http::trace.readTimeoutMs, 5000);
  expectOneRequest();
}

TEST_F(AnkiHttp, TemporaryReadStallsDoNotResendThePostOrLoseResponseBytes) {
  test_http::scenario.response = "abcdef";
  test_http::scenario.declaredLength = 6;
  test_http::scenario.readChunk = 2;
  test_http::scenario.readResults = {-ESP_ERR_HTTP_EAGAIN, 2, -ESP_ERR_HTTP_EAGAIN,
                                     -ESP_ERR_HTTP_EAGAIN, 2, -ESP_ERR_HTTP_EAGAIN};
  EXPECT_EQ(request("/push", "test-token", "POST", "{}"), Error::OK);
  EXPECT_EQ(received, "abcdef");
  EXPECT_EQ(test_http::trace.body, "{}");
  EXPECT_EQ(test_http::trace.writeCalls, 1);
  expectOneRequest();
}

TEST_F(AnkiHttp, SuccessfulReadsResetTheIdleDeadline) {
  test_http::scenario.response = "abcdef";
  test_http::scenario.declaredLength = 6;
  for (int chunk = 0; chunk < 3; ++chunk) {
    for (int poll = 0; poll < 5; ++poll) test_http::scenario.readResults.push_back(-ESP_ERR_HTTP_EAGAIN);
    test_http::scenario.readResults.push_back(2);
  }
  EXPECT_EQ(request(), Error::OK);
  EXPECT_EQ(received, "abcdef");
  EXPECT_GT(millis(), 75000U);
  expectOneRequest();
}

TEST_F(AnkiHttp, ContinuousReadStallFailsAfterThirtySecondsWithoutResending) {
  test_http::scenario.readResults.assign(6, -ESP_ERR_HTTP_EAGAIN);
  EXPECT_EQ(request("/push", "test-token", "POST", "{}"), Error::HTTP_ERROR);
  EXPECT_TRUE(received.empty());
  EXPECT_EQ(test_http::trace.body, "{}");
  EXPECT_EQ(test_http::trace.writeCalls, 1);
  EXPECT_EQ(test_http::trace.readCalls, 6);
  EXPECT_GE(millis(), 30000U);
  EXPECT_LT(millis(), 35000U);
  expectOneRequest();
}

TEST_F(AnkiHttp, CancellationDuringReadPollWinsEvenAtTheIdleDeadline) {
  for (const uint32_t cancelAt : {5000U, 30000U}) {
    SCOPED_TRACE(cancelAt);
    test_http::reset();
    test_http::scenario.readResults.assign(6, -ESP_ERR_HTTP_EAGAIN);
    EXPECT_EQ(request("/push", "test-token", "POST", "{}", 64, [cancelAt] { return millis() >= cancelAt; }),
              Error::ABORTED);
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(test_http::trace.body, "{}");
    EXPECT_EQ(test_http::trace.writeCalls, 1);
    EXPECT_EQ(test_http::trace.readCalls, static_cast<int>(cancelAt / 5000U));
    expectOneRequest();
  }
}

TEST_F(AnkiHttp, ReadTimeoutSetupFailureCleansUpWithoutReadingOrRetrying) {
  test_http::scenario.failTimeout = true;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  EXPECT_TRUE(received.empty());
  EXPECT_EQ(test_http::trace.timeoutCalls, 1);
  EXPECT_EQ(test_http::trace.readCalls, 0);
  expectOneRequest();
}

TEST_F(AnkiHttp, CallbackCanAbortWithoutRetry) {
  int calls = 0;
  EXPECT_EQ(HttpDownloader::requestToken(
                "http://desktop/pull", "test-token", "GET", {},
                [&](const uint8_t*, size_t) {
                  ++calls;
                  return false;
                },
                64, status),
            Error::FILE_ERROR);
  EXPECT_EQ(calls, 1);
  expectOneRequest();
}

TEST_F(AnkiHttp, CancellationBeforeOpenEvaluatesCallbackOnce) {
  int calls = 0;
  EXPECT_EQ(request("/pull", "test-token", "GET", {}, 64, [&]() { return ++calls == 1; }), Error::ABORTED);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(status, 0);
  EXPECT_EQ(test_http::trace.initCalls, 0);
}

TEST_F(AnkiHttp, CancellationDuringResponseStopsBeforeNextChunk) {
  int calls = 0;
  test_http::scenario.response = "abcdef";
  test_http::scenario.declaredLength = 6;
  test_http::scenario.readChunk = 2;
  EXPECT_EQ(request("/pull", "test-token", "GET", {}, 64, [&]() { return ++calls == 3; }), Error::ABORTED);
  EXPECT_EQ(received, "ab");
  EXPECT_EQ(test_http::trace.readCalls, 1);
  expectOneRequest();
}

TEST_F(AnkiHttp, CancellationDuringShortPostWritesDoesNotResend) {
  int calls = 0;
  test_http::scenario.writeChunk = 2;
  EXPECT_EQ(request("/push", "test-token", "POST", "abcdef", 64, [&]() { return ++calls == 3; }), Error::ABORTED);
  EXPECT_EQ(test_http::trace.body, "ab");
  EXPECT_EQ(test_http::trace.writeCalls, 1);
  expectOneRequest();
}

TEST_F(AnkiHttp, WriteAndHeaderFailuresCleanUpWithoutRetry) {
  test_http::scenario.failWrite = true;
  EXPECT_EQ(request("/push", "test-token", "POST", "{}"), Error::HTTP_ERROR);
  expectOneRequest();
  test_http::reset();
  test_http::scenario.failHeader = "Authorization";
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  EXPECT_EQ(test_http::trace.openCalls, 0);
  EXPECT_EQ(test_http::trace.initCalls, 1);
  EXPECT_EQ(test_http::trace.cleanupCalls, 1);
}

TEST_F(AnkiHttp, InitializationAndConnectionFailuresAreNotRetried) {
  test_http::scenario.failInit = true;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  EXPECT_EQ(test_http::trace.initCalls, 1);
  EXPECT_EQ(test_http::trace.openCalls, 0);
  EXPECT_EQ(test_http::trace.cleanupCalls, 0);
  test_http::reset();
  test_http::scenario.failOpen = true;
  EXPECT_EQ(request(), Error::HTTP_ERROR);
  expectOneRequest();
}

TEST_F(AnkiHttp, MaximumTokenLengthFitsWithoutTruncation) {
  const std::string token(128, 'a');
  EXPECT_EQ(request("/pull", token.c_str()), Error::OK);
  EXPECT_EQ(test_http::trace.headers.at("Authorization"), "Bearer " + token);
}

TEST_F(AnkiHttp, RejectsInvalidInputsBeforeOpeningConnection) {
  for (const char* token : {"bad\rvalue", "bad\nvalue", "bad\tvalue", "bad\x7fvalue"}) {
    EXPECT_EQ(request("/pull", token), Error::HTTP_ERROR);
  }
  const std::string longToken(129, 'a');
  EXPECT_EQ(request("/pull", longToken.c_str()), Error::HTTP_ERROR);
  EXPECT_EQ(request("/pull", nullptr), Error::HTTP_ERROR);
  EXPECT_EQ(request("/pull", "test-token", nullptr), Error::HTTP_ERROR);
  EXPECT_EQ(request("/pull", "test-token", "DELETE"), Error::HTTP_ERROR);
  EXPECT_EQ(request("/pull", "test-token", "GET", "unexpected"), Error::HTTP_ERROR);
  EXPECT_EQ(request("/push", "", "POST", "{}"), Error::HTTP_ERROR);
  EXPECT_EQ(request("/pull", "test-token", "GET", {}, 0), Error::HTTP_ERROR);
  const std::string oversizedBody(16 * 1024 + 1, 'a');
  EXPECT_EQ(request("/push", "test-token", "POST", oversizedBody), Error::HTTP_ERROR);
  EXPECT_EQ(HttpDownloader::requestToken("http://desktop/pull", "test-token", "GET", {}, {}, 64, status),
            Error::HTTP_ERROR);
  EXPECT_EQ(test_http::trace.initCalls, 0);
}
}  // namespace
