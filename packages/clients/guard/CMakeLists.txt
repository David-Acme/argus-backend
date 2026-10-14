cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::guard)
  argus_clients(NAME guard
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/guard/v1/presence.proto
            argus/guard/v1/duress.proto
      SOURCES src/guard/guard-presence-client.cc
              src/guard/guard-duress-client.cc
      INCLUDES src)
endif()

enable_testing()
find_package(doctest REQUIRED)

if(NOT TARGET guard-presence-client-test)
  add_executable(guard-presence-client-test
      tests/unit/guard-presence-client-test.cc)

  set_target_properties(guard-presence-client-test PROPERTIES
      EXCLUDE_FROM_ALL FALSE)

  target_link_libraries(guard-presence-client-test PRIVATE
      argus::clients::guard argus::lib::grpc-test-support argus_client_grpc_base
      doctest::doctest)

  target_compile_options(guard-presence-client-test PRIVATE -Wall -Wextra)

  add_test(NAME guard-presence-client-test
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/internal-tls-test.sh
                   $<TARGET_FILE:guard-presence-client-test> 3)
  set_tests_properties(guard-presence-client-test PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(NOT TARGET guard-duress-client-test)
  add_executable(guard-duress-client-test
      tests/unit/guard-duress-client-test.cc)

  set_target_properties(guard-duress-client-test PROPERTIES
      EXCLUDE_FROM_ALL FALSE)

  target_link_libraries(guard-duress-client-test PRIVATE
      argus::clients::guard doctest::doctest)

  target_compile_options(guard-duress-client-test PRIVATE -Wall -Wextra)

  add_test(NAME guard-duress-client-test COMMAND guard-duress-client-test)
endif()
