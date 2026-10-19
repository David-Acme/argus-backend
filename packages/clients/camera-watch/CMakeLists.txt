cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::camera-watch)
  argus_clients(NAME camera-watch
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/camera/v1/watch.proto
      SOURCES src/camera/camera-watch-client.cc
      INCLUDES src)
endif()

if(ARGUS_PACKAGE_TESTS)
    enable_testing()
    find_package(doctest REQUIRED)

    add_executable(camera-watch-client-test tests/unit/camera-watch-client-test.cc)

    set_target_properties(camera-watch-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    target_link_libraries(camera-watch-client-test PRIVATE argus::clients::camera-watch
        argus::lib::grpc-test-support argus_client_grpc_base argus::lib::doctest-main)

    target_compile_options(camera-watch-client-test PRIVATE -Wall -Wextra)

    add_test(NAME camera-watch-client-test
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/internal-tls-test.sh
                     $<TARGET_FILE:camera-watch-client-test> 3)
    set_tests_properties(camera-watch-client-test PROPERTIES SKIP_RETURN_CODE 77)
endif()
