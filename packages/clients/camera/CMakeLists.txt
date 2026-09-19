cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::client-camera)
  argus_client_module(NAME camera
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/camera/v1/sync.proto
      SOURCES src/camera/camera-sync-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(client-caller-identity-test
    tests/unit/client-caller-identity-test.cc)

# This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the client
# targets), so the suite has to opt back into the default build to be
# present when ctest runs it.
set_target_properties(client-caller-identity-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(client-caller-identity-test PRIVATE argus::client-camera
    doctest::doctest)

target_compile_options(client-caller-identity-test PRIVATE -Wall -Wextra)

add_test(NAME client-caller-identity-test COMMAND client-caller-identity-test)
