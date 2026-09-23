cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::sync)
  argus_clients(NAME sync
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/sync/v1/contracts.proto
            argus/sync/v1/sync.proto
      SOURCES src/sync/sync-client.cc
      INCLUDES src
      DEPENDS
          argus::contracts::sync
          argus::lib::text)
endif()

enable_testing()
find_package(doctest REQUIRED)

if(NOT TARGET sync-client-test)
  add_executable(sync-client-test tests/unit/sync-client-test.cc)

  set_target_properties(sync-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

  target_link_libraries(sync-client-test PRIVATE argus::clients::sync
      doctest::doctest)

  target_compile_options(sync-client-test PRIVATE -Wall -Wextra)

  add_test(NAME sync-client-test COMMAND sync-client-test)
endif()
