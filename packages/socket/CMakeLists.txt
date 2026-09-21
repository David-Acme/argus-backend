cmake_minimum_required(VERSION 3.25)
project(argus-socket VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_socket: the per-service WebSocket emitter (SocketService) and the
# socket event shapes (emit DTO plus user/camera change sinks). Consumers
# link argus::socket.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# Found before the sibling add_subdirectory block: argus-room links Drogon
# from this scope.
find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

# The packages this module links, declared here rather than by the consumer:
# every guard keeps the root tree and every other consumer in charge.
if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/text
                     ${CMAKE_BINARY_DIR}/text)
endif()

if(NOT TARGET argus::room)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../room
                     ${CMAKE_BINARY_DIR}/room)
endif()

if(NOT TARGET argus::lib::nats)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/nats
                     ${CMAKE_BINARY_DIR}/nats)
endif()

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

argus_module(NAME socket
    SOURCES
        src/shared/services/socket/socket-service.cc
        src/shared/dtos/socket-emit/socket-emit-dto.cc
        src/shared/dtos/socket-emit/socket-emit-dto.hxx
        src/shared/contracts/user-change-sink.hxx
        src/shared/contracts/camera-change-sink.hxx
    INCLUDES src)

target_link_libraries(argus_socket PUBLIC
    argus::room
    argus::lib::nats
    argus::contracts::sync
    argus::lib::text
    Drogon::Drogon
    nlohmann_json::nlohmann_json)

target_compile_options(argus_socket PRIVATE -Wall -Wextra)

# The sync-change payload shape the socket module owns.
find_package(doctest REQUIRED)

add_executable(sync-change-test tests/unit/sync-change-test.cc)
target_link_libraries(sync-change-test PRIVATE
    argus::socket
    argus::lib::text
    argus::lib::nats
    argus::contracts::auth
    argus::contracts::sync
    doctest::doctest)
target_compile_options(sync-change-test PRIVATE -Wall -Wextra)
add_test(NAME sync-change-test COMMAND sync-change-test)
