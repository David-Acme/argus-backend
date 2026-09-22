cmake_minimum_required(VERSION 3.25)
project(argus-gateway VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GATEWAY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus_identity)
    # Standalone configure: this service owns the whole build, so it adds the
    # packages it links — the leaves it uses directly and the sibling packages
    # that used to arrive through argus-common — plus the third-party sources
    # so the gateway links the same objects as any other consumer.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()

    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()

    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::text)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/text
                         ${CMAKE_BINARY_DIR}/text)
    endif()
    if(NOT TARGET argus::lib::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()

    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
    set(NCNN_AVX2 ON CACHE BOOL "" FORCE)
    set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
    set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ncnn
                     ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    set(ARGUS_NCNN_TARGET ncnn)

    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
    endif()
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity
                         ${CMAKE_BINARY_DIR}/identity)
    endif()
    if(NOT TARGET identity-migration)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity/tools/migrate-identity
                         ${CMAKE_BINARY_DIR}/identity/tools/migrate-identity)
    endif()
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/sync
                     ${CMAKE_BINARY_DIR}/clients/sync EXCLUDE_FROM_ALL)
endif()

# mdns is its own package again (Phase 1 step 10): the source was gateway-only,
# so gateway-core links argus::lib::mdns, and the vendored mdns.h wiring lives there.
if(NOT TARGET argus::lib::mdns)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/mdns
                     ${CMAKE_BINARY_DIR}/mdns)
endif()

find_package(doctest REQUIRED)

if(NOT TARGET argus::contracts::gateway)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/gateway
                     ${CMAKE_BINARY_DIR}/contracts/gateway)
endif()

add_library(gateway-core STATIC
    ${GATEWAY_SRC_ROOT}/identity/identity-config.cc
    ${GATEWAY_SRC_ROOT}/identity/identity-registrar.cc
    ${GATEWAY_SRC_ROOT}/proxy/proxy-config.cc
    ${GATEWAY_SRC_ROOT}/proxy/reverse-proxy.cc
    ${GATEWAY_SRC_ROOT}/server/refresh-rate-limiter.cc
    ${GATEWAY_SRC_ROOT}/server/remote-config.cc
    ${GATEWAY_SRC_ROOT}/server/remote-gate.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-notifier.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-stream-relay.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-stream-socket.cc
    ${GATEWAY_SRC_ROOT}/sync/fallback/fallback-log-repository.cc
)

target_include_directories(gateway-core PUBLIC ${GATEWAY_SRC_ROOT})

target_link_libraries(gateway-core PUBLIC argus_identity
    argus::lib::cert argus::lib::mdns argus::lib::http
    argus::clients::identity argus::contracts::auth argus::contracts::sync
    argus::clients::notification argus::clients::sync)
target_link_libraries(gateway-core PRIVATE argus::contracts::gateway argus::contracts::auth argus::contracts::sync)

target_compile_options(gateway-core PRIVATE -Wall -Wextra)

add_executable(argus-gateway ${GATEWAY_SRC_ROOT}/main.cc)

# Whole-archive: the vendored proxy plugin self-registers through a template
# static initializer that nothing references by name, so a plain static-lib
# link would drop the registration object.
target_link_libraries(argus-gateway PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,gateway-core>)

target_compile_options(argus-gateway PRIVATE -Wall -Wextra)

argus_runtime_rpath(argus-gateway)

add_executable(gateway-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway-test.cc)

target_include_directories(gateway-test PRIVATE ${GATEWAY_SRC_ROOT})

target_link_libraries(gateway-test PRIVATE gateway-core doctest::doctest argus::contracts::auth)

target_compile_options(gateway-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(gateway-test)

add_executable(camera-notifier-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/camera-notifier-test.cc)

target_include_directories(camera-notifier-test PRIVATE ${GATEWAY_SRC_ROOT})

target_compile_definitions(camera-notifier-test PRIVATE
    ARGUS_GATEWAY_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(camera-notifier-test PRIVATE gateway-core
                                                   doctest::doctest)

target_compile_options(camera-notifier-test PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# The suites opt back into the default build: their folder is added
# EXCLUDE_FROM_ALL, and a registered test whose binary never builds is a
# ctest "Not Run", not a pass.
set_target_properties(gateway-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(camera-notifier-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME gateway-test COMMAND gateway-test)
add_test(NAME camera-notifier-test COMMAND camera-notifier-test)
