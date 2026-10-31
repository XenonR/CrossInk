#pragma once
template <typename... Args>
inline void testHttpLog(Args&&...) {}
#define LOG_DBG(...) testHttpLog(__VA_ARGS__)
#define LOG_ERR(...) testHttpLog(__VA_ARGS__)
#define LOG_INF(...) testHttpLog(__VA_ARGS__)
