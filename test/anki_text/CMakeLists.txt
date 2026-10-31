add_executable(AnkiTextLayoutTest
  AnkiTextLayoutTest.cpp
  ${REPO_ROOT}/src/anki/AnkiTextLayout.cpp
)
target_include_directories(AnkiTextLayoutTest PRIVATE ${REPO_ROOT}/src)
target_link_libraries(AnkiTextLayoutTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AnkiTextLayoutTest)
