cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::camera-actions)
  argus_clients(NAME camera-actions
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/camera/v1/actions.proto
      SOURCES src/camera/camera-action-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(camera-action-client-test tests/unit/camera-action-client-test.cc)

set_target_properties(camera-action-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(camera-action-client-test PRIVATE argus::clients::camera-actions
    doctest::doctest)

target_compile_options(camera-action-client-test PRIVATE -Wall -Wextra)

add_test(NAME camera-action-client-test COMMAND camera-action-client-test)
