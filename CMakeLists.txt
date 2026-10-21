cmake_minimum_required(VERSION 3.14)
project(minilsm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch GoogleTest at configure time
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

include(GoogleTest)

add_executable(smoke_test tests/smoke_test.cc)
target_link_libraries(smoke_test GTest::gtest_main)
gtest_discover_tests(smoke_test)

add_executable(status_test tests/status_test.cc)
target_include_directories(status_test PRIVATE include)
target_link_libraries(status_test GTest::gtest_main)
gtest_discover_tests(status_test)

add_library(minilsm
  src/memtable.cc
  src/lsm_db.cc
  src/wal_record.cc
  src/crc32.cc
  src/wal_writer.cc
  src/wal_reader.cc
  src/sstable_format.cc
  src/sstable_writer.cc
  src/sstable_reader.cc
  src/bloomfilter.cc
  src/MANIFEST.cc
  src/Manifest_reader.cc
  src/Manifest_writer.cc)
target_include_directories(minilsm PUBLIC include)

add_executable(memtable_test tests/memtable_test.cc)
target_link_libraries(memtable_test minilsm GTest::gtest_main)
gtest_discover_tests(memtable_test)

add_executable(lsm_db_test tests/lsm_db_test.cc)
target_link_libraries(lsm_db_test minilsm GTest::gtest_main)
gtest_discover_tests(lsm_db_test)

add_executable(wal_record_test tests/wal_record_test.cc)
target_link_libraries(wal_record_test minilsm GTest::gtest_main)
gtest_discover_tests(wal_record_test)

add_executable(crc32_test tests/crc32_test.cc)
target_link_libraries(crc32_test minilsm GTest::gtest_main)
gtest_discover_tests(crc32_test)

add_executable(wal_writer_test tests/wal_writer_test.cc)
target_link_libraries(wal_writer_test minilsm GTest::gtest_main)
gtest_discover_tests(wal_writer_test)

add_executable(wal_reader_test tests/wal_reader_test.cc)
target_link_libraries(wal_reader_test minilsm GTest::gtest_main)
gtest_discover_tests(wal_reader_test)

add_executable(lsm_db_recovery_test tests/lsm_db_recovery_test.cc)
target_link_libraries(lsm_db_recovery_test minilsm GTest::gtest_main)
gtest_discover_tests(lsm_db_recovery_test)

add_executable(sstable_format_test tests/sstable_format_test.cc)
target_link_libraries(sstable_format_test minilsm GTest::gtest_main)
gtest_discover_tests(sstable_format_test)

add_executable(sstable_writer_test tests/sstable_writer_test.cc)
target_link_libraries(sstable_writer_test minilsm GTest::gtest_main)
gtest_discover_tests(sstable_writer_test)

add_executable(sstable_reader_test tests/sstable_reader_test.cc)
target_link_libraries(sstable_reader_test minilsm GTest::gtest_main)
gtest_discover_tests(sstable_reader_test)

add_executable(snapshot_test tests/memtable_snapshot_test.cc)
target_link_libraries(snapshot_test minilsm GTest::gtest_main)
gtest_discover_tests(snapshot_test) 

add_executable(lsmdb_flush_test tests/lsmdb_flush_test.cc)
target_link_libraries(lsmdb_flush_test minilsm GTest::gtest_main)
gtest_discover_tests(lsmdb_flush_test)

add_executable(lsmdb_async_test tests/lsmdb_async_test.cc)
target_link_libraries(lsmdb_async_test minilsm GTest::gtest_main)
gtest_discover_tests(lsmdb_async_test)

add_executable(sstable_test tests/sstable_test.cc)
target_link_libraries(sstable_test minilsm GTest::gtest_main)
gtest_discover_tests(sstable_test)

add_executable(bloomfilter_test tests/bloomfilter_test.cc)
target_link_libraries(bloomfilter_test minilsm GTest::gtest_main)
gtest_discover_tests(bloomfilter_test)

add_executable(manifest_test tests/manifest_test.cc)
target_link_libraries(manifest_test minilsm GTest::gtest_main)
gtest_discover_tests(manifest_test)
