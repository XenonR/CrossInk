add_executable(AnkiHttpTest
  AnkiHttpTest.cpp
  ${REPO_ROOT}/src/network/HttpDownloader.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
)
target_include_directories(AnkiHttpTest PRIVATE
  stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/AppVersion
)
# Exercise the device path, including incremental POST writes, rather than the
# simulator shim which stages and buffers requests before returning open().
target_compile_definitions(AnkiHttpTest PRIVATE CROSSINK_APP_CAP_ANKI=1)
target_link_libraries(AnkiHttpTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AnkiHttpTest)
