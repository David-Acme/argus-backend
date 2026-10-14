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
            argus/identity/v1/sync.proto
            argus/identity/v1/voiceprint.proto
      SOURCES src/identity/identity-client.cc
              src/identity/identity-sync-client.cc
              src/identity/voiceprint-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(identity-grpc-client-test tests/unit/identity-grpc-client-test.cc)

set_target_properties(identity-grpc-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(identity-grpc-client-test PRIVATE argus::clients::identity
    argus::lib::grpc-test-support argus_client_grpc_base doctest::doctest)

target_compile_options(identity-grpc-client-test PRIVATE -Wall -Wextra)

add_test(NAME identity-grpc-client-test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/internal-tls-test.sh
                 $<TARGET_FILE:identity-grpc-client-test> 5)
set_tests_properties(identity-grpc-client-test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(identity-voiceprint-client-test
    tests/unit/voiceprint-client-test.cc)

set_target_properties(identity-voiceprint-client-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(identity-voiceprint-client-test PRIVATE
    argus::clients::identity argus::lib::grpc-test-support argus_client_grpc_base
    doctest::doctest)

target_compile_options(identity-voiceprint-client-test PRIVATE -Wall -Wextra)

add_test(NAME identity-voiceprint-client-test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/internal-tls-test.sh
                 $<TARGET_FILE:identity-voiceprint-client-test> 3)
set_tests_properties(identity-voiceprint-client-test PROPERTIES SKIP_RETURN_CODE 77)
