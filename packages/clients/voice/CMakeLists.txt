cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::clients::voice)
  argus_clients(NAME voice
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/voice/v1/voice.proto
      SOURCES src/voice/voice-client.cc
      INCLUDES src
      DEPENDS
          argus::contracts::auth)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(voice-client-test tests/unit/voice-client-test.cc)

set_target_properties(voice-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(voice-client-test PRIVATE argus::clients::voice
    doctest::doctest)

target_compile_options(voice-client-test PRIVATE -Wall -Wextra)

add_test(NAME voice-client-test COMMAND voice-client-test)
