cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::notification)
  argus_clients(NAME notification
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/notification/v1/notification.proto
      SOURCES src/notification/notification-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(notification-client-test tests/unit/notification-client-test.cc)

# This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the client
# targets), so the suite has to opt back into the default build to be present
# when ctest runs it.
set_target_properties(notification-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-client-test PRIVATE argus::clients::notification
    doctest::doctest)

target_compile_options(notification-client-test PRIVATE -Wall -Wextra)

add_test(NAME notification-client-test COMMAND notification-client-test)
