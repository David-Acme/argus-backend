cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::identity)
  argus_clients(NAME identity
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/identity/v1/identity.proto
      SOURCES src/identity/identity-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(identity-grpc-client-test tests/unit/identity-grpc-client-test.cc)

set_target_properties(identity-grpc-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(identity-grpc-client-test PRIVATE argus::clients::identity
    doctest::doctest)

target_compile_options(identity-grpc-client-test PRIVATE -Wall -Wextra)

add_test(NAME identity-grpc-client-test COMMAND identity-grpc-client-test)
