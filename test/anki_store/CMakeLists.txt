add_executable(AnkiStoreTest AnkiStoreTest.cpp ${REPO_ROOT}/src/anki/AnkiStore.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(AnkiStoreTest PRIVATE stubs ${REPO_ROOT}/src/anki ${REPO_ROOT}/include
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(AnkiStoreTest PRIVATE CROSSINK_APP_CAP_ANKI=1 CROSSINK_APP_CAP_TOUCH=1 CROSSINK_APP_CAP_USB_DRIVE=1 SIMULATOR=1)
target_link_libraries(AnkiStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AnkiStoreTest)
