add_executable(AnkiSyncTest
  AnkiSyncTest.cpp
  ${REPO_ROOT}/src/anki/AnkiSyncClient.cpp
  ${REPO_ROOT}/src/anki/AnkiStore.cpp
  ${REPO_ROOT}/src/anki/AnkiProtocol.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(AnkiSyncTest PRIVATE
  stubs
  ${REPO_ROOT}/test/anki_store/stubs
  ${REPO_ROOT}/src/anki
  ${REPO_ROOT}/include
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/uzlib/src
)
target_compile_definitions(AnkiSyncTest PRIVATE
  CROSSINK_APP_CAP_ANKI=1 CROSSINK_APP_CAP_TOUCH=1 CROSSINK_APP_CAP_USB_DRIVE=1 SIMULATOR=1
)
target_link_libraries(AnkiSyncTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(AnkiSyncTest)

add_executable(AnkiConnectionStoreTest
  AnkiConnectionStoreTest.cpp
  ${REPO_ROOT}/src/anki/AnkiConnectionStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
)
target_include_directories(AnkiConnectionStoreTest PRIVATE
  stubs
  ${REPO_ROOT}/test/anki_store/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/anki
  ${REPO_ROOT}/include
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(AnkiConnectionStoreTest PRIVATE
  CROSSINK_APP_CAP_ANKI=1 CROSSINK_APP_CAP_TOUCH=1 CROSSINK_APP_CAP_USB_DRIVE=1 SIMULATOR=1
)
target_link_libraries(AnkiConnectionStoreTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(AnkiConnectionStoreTest)
