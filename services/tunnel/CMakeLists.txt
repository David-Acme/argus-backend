cmake_minimum_required(VERSION 3.25)
project(argus-tunnel VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)
argus_queue_skipped_test_count()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TUNNEL_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::text)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/text
                         ${CMAKE_BINARY_DIR}/text)
    endif()
    if(NOT TARGET argus::contracts::push)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/push
                         ${CMAKE_BINARY_DIR}/push)
    endif()
endif()

find_package(doctest REQUIRED)

add_library(tunnel-core STATIC
    ${TUNNEL_SRC_ROOT}/net/poll-loop.cc
    ${TUNNEL_SRC_ROOT}/net/tcp-listener.cc
    ${TUNNEL_SRC_ROOT}/net/tcp-peer.cc
    ${TUNNEL_SRC_ROOT}/protocol/frames.cc
    ${TUNNEL_SRC_ROOT}/link/protocol/frames.cc
    ${TUNNEL_SRC_ROOT}/link/protocol/credit-window.cc
    ${TUNNEL_SRC_ROOT}/link/protocol/client-hello.cc
    ${TUNNEL_SRC_ROOT}/link/tls/pinned-key.cc
    ${TUNNEL_SRC_ROOT}/link/tls/client-tls-context.cc
    ${TUNNEL_SRC_ROOT}/link/tls/tls-channel.cc
    ${TUNNEL_SRC_ROOT}/link/tls/host-resolver.cc
    ${TUNNEL_SRC_ROOT}/link/client/home-service.cc
    ${TUNNEL_SRC_ROOT}/link/client/link-client.cc
    ${TUNNEL_SRC_ROOT}/link/push/sealed-push-bridge.cc
    ${TUNNEL_SRC_ROOT}/link/status/link-status.cc
    ${TUNNEL_SRC_ROOT}/core/tunnel-mux.cc
    ${TUNNEL_SRC_ROOT}/client/tunnel-client.cc
    ${TUNNEL_SRC_ROOT}/relay/tunnel-relay.cc
    ${TUNNEL_SRC_ROOT}/server/service-config.cc
    ${TUNNEL_SRC_ROOT}/server/health-extras.cc
)

target_include_directories(tunnel-core PUBLIC ${TUNNEL_SRC_ROOT})

target_link_libraries(tunnel-core PUBLIC argus::lib::config argus::lib::http
    argus::lib::runtime argus::lib::nats argus::lib::text argus::contracts::routes
    argus::contracts::push OpenSSL::SSL OpenSSL::Crypto)

target_compile_options(tunnel-core PRIVATE -Wall -Wextra)

add_executable(argus-tunnel-client ${TUNNEL_SRC_ROOT}/main-client.cc)
target_link_libraries(argus-tunnel-client PRIVATE tunnel-core argus::lib::nats)
target_compile_options(argus-tunnel-client PRIVATE -Wall -Wextra)

add_executable(argus-tunnel-relay ${TUNNEL_SRC_ROOT}/main-relay.cc)
target_link_libraries(argus-tunnel-relay PRIVATE tunnel-core argus::lib::nats)
target_compile_options(argus-tunnel-relay PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(framing-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/framing-test.cc)
target_link_libraries(framing-test PRIVATE tunnel-core argus::lib::doctest-main)
target_compile_options(framing-test PRIVATE -Wall -Wextra)

add_executable(tunnel-integration-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-integration-test.cc)
target_include_directories(tunnel-integration-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-integration-test PRIVATE tunnel-core
                                                      argus::lib::doctest-main)
target_compile_options(tunnel-integration-test PRIVATE -Wall -Wextra)

add_executable(tunnel-reconnect-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-reconnect-test.cc)
target_include_directories(tunnel-reconnect-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-reconnect-test PRIVATE tunnel-core
                                                    argus::lib::doctest-main)
target_compile_options(tunnel-reconnect-test PRIVATE -Wall -Wextra)

add_executable(tunnel-negauth-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-negauth-test.cc)
target_include_directories(tunnel-negauth-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-negauth-test PRIVATE tunnel-core
                                                  argus::lib::doctest-main)
target_compile_options(tunnel-negauth-test PRIVATE -Wall -Wextra)

add_executable(tunnel-hardening-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-hardening-test.cc)
target_include_directories(tunnel-hardening-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-hardening-test PRIVATE tunnel-core
                                                    argus::lib::doctest-main)
target_compile_options(tunnel-hardening-test PRIVATE -Wall -Wextra)

add_executable(service-config-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/service-config-test.cc)
target_link_libraries(service-config-test PRIVATE tunnel-core
                                                  argus::lib::doctest-main)
target_compile_options(service-config-test PRIVATE -Wall -Wextra)


add_executable(tunnel-health-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel-health-test.cc)
target_include_directories(tunnel-health-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tunnel-health-test PRIVATE tunnel-core argus::lib::doctest-main)
target_compile_options(tunnel-health-test PRIVATE -Wall -Wextra)

execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --path-format=absolute --git-common-dir
    OUTPUT_VARIABLE ARGUS_GIT_COMMON_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
get_filename_component(ARGUS_MAIN_CHECKOUT "${ARGUS_GIT_COMMON_DIR}/.." ABSOLUTE)
get_filename_component(ARGUS_CLOUD_CHECKOUT "${ARGUS_MAIN_CHECKOUT}/../argus-cloud" ABSOLUTE)

add_executable(link-protocol-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-protocol-test.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tls-hello.cc)
target_include_directories(link-protocol-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(link-protocol-test PRIVATE tunnel-core argus::lib::doctest-main
                                            nlohmann_json::nlohmann_json)
target_compile_options(link-protocol-test PRIVATE -Wall -Wextra)
target_compile_definitions(link-protocol-test PRIVATE
    ARGUS_LINK_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors"
    ARGUS_CLOUD_LINK_VECTOR_DIR="${ARGUS_CLOUD_CHECKOUT}/tests/protocol/vectors")

add_executable(link-client-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-client-test.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-harness.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-test-cert.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/tls-hello.cc)
target_include_directories(link-client-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(link-client-test PRIVATE tunnel-core argus::lib::doctest-main)
target_compile_options(link-client-test PRIVATE -Wall -Wextra)

add_executable(cloud-interop-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/cloud-interop-test.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop-support.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-harness.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-test-cert.cc)
target_include_directories(cloud-interop-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(cloud-interop-test PRIVATE tunnel-core argus::lib::doctest-main)
target_compile_options(cloud-interop-test PRIVATE -Wall -Wextra)

add_executable(link-status-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-status-test.cc)
target_link_libraries(link-status-test PRIVATE tunnel-core argus::lib::doctest-main)
target_compile_options(link-status-test PRIVATE -Wall -Wextra)

add_executable(home-service-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/home-service-test.cc)
target_link_libraries(home-service-test PRIVATE tunnel-core argus::lib::doctest-main)
target_compile_options(home-service-test PRIVATE -Wall -Wextra)

add_executable(sealed-push-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/sealed-push-test.cc
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/link-test-cert.cc)
target_include_directories(sealed-push-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(sealed-push-test PRIVATE tunnel-core argus::lib::doctest-main
                                               nlohmann_json::nlohmann_json
                                               OpenSSL::Crypto)
target_compile_options(sealed-push-test PRIVATE -Wall -Wextra)
target_compile_definitions(sealed-push-test PRIVATE
    ARGUS_SEALED_PUSH_VECTOR_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/push-sealed.json"
    ARGUS_CLOUD_SEALED_PUSH_VECTOR_FILE="${ARGUS_CLOUD_CHECKOUT}/tests/vectors/push-sealed.json")

set_target_properties(framing-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(tunnel-integration-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(tunnel-reconnect-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(tunnel-negauth-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(tunnel-hardening-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(service-config-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(tunnel-health-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(link-protocol-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(link-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(cloud-interop-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(link-status-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(home-service-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(sealed-push-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME framing-test COMMAND framing-test)
add_test(NAME tunnel-integration-test COMMAND tunnel-integration-test)
add_test(NAME tunnel-reconnect-test COMMAND tunnel-reconnect-test)
add_test(NAME tunnel-negauth-test COMMAND tunnel-negauth-test)
add_test(NAME tunnel-hardening-test COMMAND tunnel-hardening-test)
add_test(NAME service-config-test COMMAND service-config-test)
add_test(NAME tunnel-health-test COMMAND tunnel-health-test)
add_test(NAME link-protocol-test COMMAND link-protocol-test)
add_test(NAME link-client-test COMMAND link-client-test)
add_test(NAME cloud-interop-test COMMAND cloud-interop-test)
add_test(NAME link-status-test COMMAND link-status-test)
add_test(NAME home-service-test COMMAND home-service-test)
add_test(NAME sealed-push-test COMMAND sealed-push-test)
