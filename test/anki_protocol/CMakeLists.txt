add_executable(AnkiProtocolTest
  AnkiProtocolTest.cpp
  ${REPO_ROOT}/src/anki/AnkiProtocol.cpp
)
target_include_directories(AnkiProtocolTest PRIVATE
  stubs
  ${REPO_ROOT}/src/anki
  ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(AnkiProtocolTest PRIVATE CROSSINK_APP_CAP_ANKI=1)
target_link_libraries(AnkiProtocolTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(AnkiProtocolTest)
