cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::productivity)
  argus_clients(NAME productivity
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/productivity/v1/sync.proto argus/productivity/v1/reminder.proto
      SOURCES src/productivity/productivity-sync-client.cc
              src/productivity/productivity-reminder-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(productivity-sync-client-test tests/unit/productivity-sync-client-test.cc)

set_target_properties(productivity-sync-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(productivity-sync-client-test PRIVATE argus::clients::productivity
    doctest::doctest)

target_compile_options(productivity-sync-client-test PRIVATE -Wall -Wextra)

add_test(NAME productivity-sync-client-test COMMAND productivity-sync-client-test)
