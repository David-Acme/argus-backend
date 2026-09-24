cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::auth)
  argus_clients(NAME auth
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/auth/v1/auth.proto
      SOURCES src/auth/auth-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(auth-grpc-client-test tests/unit/auth-grpc-client-test.cc)

set_target_properties(auth-grpc-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(auth-grpc-client-test PRIVATE argus::clients::auth
    doctest::doctest)

target_compile_options(auth-grpc-client-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(auth-grpc-client-test)

add_test(NAME auth-grpc-client-test COMMAND auth-grpc-client-test)
