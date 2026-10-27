cmake_minimum_required(VERSION 3.16)
project(MiniDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIDB_BUILD_TESTS "Build the integration tests" ON)
option(MINIDB_BUILD_SHARED "Build the shared library" ON)
option(MINIDB_WARNINGS "Enable compiler warnings" ON)

find_package(Threads REQUIRED)

set(MINIDB_CORE_SOURCES
    src/common/crc32c.cpp
    src/common/logger.cpp
    src/common/util.cpp
    src/storage/disk_manager.cpp
    src/buffer/buffer_pool.cpp
    src/log/log_manager.h
    src/txn/transaction.h
    src/txn/txn_manager.h
    src/space/free_space_map.cpp
    src/space/space_manager.cpp
    src/space/segment.cpp
    src/record/value.cpp
    src/record/schema.cpp
    src/record/tuple.cpp
    src/table/heap_page.cpp
    src/table/table_heap.cpp
    src/catalog/catalog.cpp
    src/sql/parser.cpp
    src/db/database.cpp
    src/db/pragma.cpp
    src/capi/minidb_c_api.cpp
)

add_library(minidb_objects OBJECT ${MINIDB_CORE_SOURCES})
set_target_properties(minidb_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(minidb_objects
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(minidb_objects PUBLIC Threads::Threads)

if(MINIDB_WARNINGS)
  target_compile_options(minidb_objects PRIVATE -Wall -Wextra)
endif()

add_library(minidb_core STATIC $<TARGET_OBJECTS:minidb_objects>)
target_include_directories(minidb_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(minidb_core PUBLIC Threads::Threads)

if(MINIDB_BUILD_SHARED)
  add_library(minidb SHARED $<TARGET_OBJECTS:minidb_objects>)
  set_target_properties(minidb PROPERTIES
      VERSION ${PROJECT_VERSION}
      SOVERSION ${PROJECT_VERSION_MAJOR}
      OUTPUT_NAME minidb
  )
  target_include_directories(minidb
      PUBLIC
          ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(minidb PUBLIC Threads::Threads)
endif()

add_executable(minidb_shell tools/shell/main.cpp)
target_link_libraries(minidb_shell PRIVATE minidb_core)

add_executable(minidb_demo examples/demo.cpp)
if(MINIDB_BUILD_SHARED)
  target_link_libraries(minidb_demo PRIVATE minidb)
else()
  target_link_libraries(minidb_demo PRIVATE minidb_core)
endif()
target_compile_definitions(minidb_demo PRIVATE
    MINIDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(MINIDB_BUILD_TESTS)
  enable_testing()
  add_executable(minidb_tests tests/test_minidb.cpp)
  target_link_libraries(minidb_tests PRIVATE minidb_core)
  target_compile_definitions(minidb_tests PRIVATE
      MINIDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME minidb_integration COMMAND minidb_tests)

  add_executable(minidb_storage_tests tests/test_storage.cpp)
  target_link_libraries(minidb_storage_tests PRIVATE minidb_core)
  add_test(NAME minidb_storage COMMAND minidb_storage_tests)

  # Manual check against real data (not a ctest: needs the TPC-H .tbl files).
  add_executable(minidb_verify_tpch tests/verify_tpch.cpp)
  target_link_libraries(minidb_verify_tpch PRIVATE minidb_core)
endif()

install(TARGETS minidb_shell minidb_demo
        RUNTIME DESTINATION bin)
if(MINIDB_BUILD_SHARED)
  install(TARGETS minidb
          LIBRARY DESTINATION lib
          ARCHIVE DESTINATION lib)
endif()
install(TARGETS minidb_core ARCHIVE DESTINATION lib)
install(FILES include/minidb/minidb.h DESTINATION include/minidb)
