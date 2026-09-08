cmake_minimum_required(VERSION 3.25)
project(argus-tunnel VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TUNNEL_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(cnats CONFIG REQUIRED)
endif()

find_package(doctest REQUIRED)

# Tunnel core: epoll engine, framing, multiplexer, client, relay and the
# /health plumbing. No thread-per-stream: everything runs on one PollLoop.
add_library(tunnel-core STATIC
    ${TUNNEL_SRC_ROOT}/net/poll-loop.cc
    ${TUNNEL_SRC_ROOT}/net/tcp-listener.cc
    ${TUNNEL_SRC_ROOT}/net/tcp-peer.cc
    ${TUNNEL_SRC_ROOT}/protocol/frames.cc
    ${TUNNEL_SRC_ROOT}/core/tunnel-mux.cc
    ${TUNNEL_SRC_ROOT}/client/tunnel-client.cc
    ${TUNNEL_SRC_ROOT}/relay/tunnel-relay.cc
    ${TUNNEL_SRC_ROOT}/server/service-config.cc
)

target_include_directories(tunnel-core PUBLIC ${TUNNEL_SRC_ROOT})

target_link_libraries(tunnel-core PUBLIC argus_common)

target_compile_options(tunnel-core PRIVATE -Wall -Wextra)

add_executable(argus-tunnel-client ${TUNNEL_SRC_ROOT}/main-client.cc)
target_link_libraries(argus-tunnel-client PRIVATE tunnel-core)
target_compile_options(argus-tunnel-client PRIVATE -Wall -Wextra)

add_executable(argus-tunnel-relay ${TUNNEL_SRC_ROOT}/main-relay.cc)
target_link_libraries(argus-tunnel-relay PRIVATE tunnel-core)
target_compile_options(argus-tunnel-relay PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(framing-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/framing-test.cc)
target_link_libraries(framing-test PRIVATE tunnel-core doctest::doctest)
target_compile_options(framing-test PRIVATE -Wall -Wextra)

add_executable(tunnel-integration-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-integration-test.cc)
target_include_directories(tunnel-integration-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-integration-test PRIVATE tunnel-core
                                                      doctest::doctest)
target_compile_options(tunnel-integration-test PRIVATE -Wall -Wextra)

add_executable(tunnel-reconnect-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-reconnect-test.cc)
target_include_directories(tunnel-reconnect-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-reconnect-test PRIVATE tunnel-core
                                                    doctest::doctest)
target_compile_options(tunnel-reconnect-test PRIVATE -Wall -Wextra)

add_executable(tunnel-negauth-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-negauth-test.cc)
target_include_directories(tunnel-negauth-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-negauth-test PRIVATE tunnel-core
                                                  doctest::doctest)
target_compile_options(tunnel-negauth-test PRIVATE -Wall -Wextra)

add_executable(service-config-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/service-config-test.cc)
target_link_libraries(service-config-test PRIVATE tunnel-core
                                                  doctest::doctest)
target_compile_options(service-config-test PRIVATE -Wall -Wextra)

add_executable(tunnel-push-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-push-test.cc)
target_include_directories(tunnel-push-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-push-test PRIVATE tunnel-core doctest::doctest)
target_compile_options(tunnel-push-test PRIVATE -Wall -Wextra)

# Test registration is standalone-only so the root project's ctest stays at
# its 38 backend cases.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_test(NAME framing-test COMMAND framing-test)
    add_test(NAME tunnel-integration-test COMMAND tunnel-integration-test)
    add_test(NAME tunnel-reconnect-test COMMAND tunnel-reconnect-test)
    add_test(NAME tunnel-negauth-test COMMAND tunnel-negauth-test)
    add_test(NAME service-config-test COMMAND service-config-test)
    add_test(NAME tunnel-push-test COMMAND tunnel-push-test)
endif()
