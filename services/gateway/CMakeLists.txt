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
    # The substrate still finds the vendored gRPC and would build the cq
    # bridge, whose entry symbol then interposes the identically-named
    # implementation inside libgrpc. Empty stand-ins skip it.
    if(NOT TARGET argus_client_grpc_bridge_entry)
        add_library(argus_client_grpc_bridge_entry INTERFACE)
        add_library(argus_client_grpc_bridge_exit INTERFACE)
    endif()
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    if(NOT TARGET argus::access)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/access
                         ${CMAKE_BINARY_DIR}/access)
    endif()
    if(NOT TARGET argus::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::hash)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/hash
                         ${CMAKE_BINARY_DIR}/hash)
    endif()
    if(NOT TARGET argus::json)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/json
                         ${CMAKE_BINARY_DIR}/json)
    endif()
    if(NOT TARGET argus::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::threading)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/threading
                         ${CMAKE_BINARY_DIR}/threading)
    endif()
    if(NOT TARGET pkg::response)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/response
                         ${CMAKE_BINARY_DIR}/response)
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

    if(NOT TARGET argus::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/cert
                         ${CMAKE_BINARY_DIR}/cert)
    endif()
    if(NOT TARGET argus::room)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/room
                         ${CMAKE_BINARY_DIR}/room)
    endif()
    if(NOT TARGET argus::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::socket)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/socket
                         ${CMAKE_BINARY_DIR}/socket)
    endif()
    if(NOT TARGET argus::audit)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/audit
                         ${CMAKE_BINARY_DIR}/audit)
    endif()
    if(NOT TARGET argus::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/auth
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
    if(NOT TARGET argus_sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/sync
                         ${CMAKE_BINARY_DIR}/sync)
    endif()
endif()

if(NOT TARGET argus::client-voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/voice
                     ${CMAKE_BINARY_DIR}/clients/voice EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::client-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::client-camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                     ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::client-productivity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/productivity
                     ${CMAKE_BINARY_DIR}/clients/productivity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::client-notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()

# mdns is gateway-only since f7-8 (nothing else ever linked argus::mdns), so
# its source compiles into gateway-core and its third-party wiring lives here.
find_package(mdns REQUIRED)

# Third-party mdns.h is header-only: treat its include dir as SYSTEM so
# -Wall -Wextra never reports warnings from the vendored header.
if(TARGET mdns::mdns)
    get_target_property(_argus_mdns_dirs mdns::mdns
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(_argus_mdns_dirs)
        set_target_properties(mdns::mdns PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_argus_mdns_dirs}")
    endif()
endif()

find_package(doctest REQUIRED)

if(NOT TARGET contract::gateway)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/gateway-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/gateway-contract)
endif()

add_library(gateway-core STATIC
    ${GATEWAY_SRC_ROOT}/identity/identity-config.cc
    # mdns is gateway-only, so it is a source here rather than a module.
    ${GATEWAY_SRC_ROOT}/shared/services/mdns/mdns-service.cc
    ${GATEWAY_SRC_ROOT}/identity/identity-registrar.cc
    ${GATEWAY_SRC_ROOT}/proxy/proxy-config.cc
    ${GATEWAY_SRC_ROOT}/proxy/reverse-proxy.cc
    ${GATEWAY_SRC_ROOT}/server/refresh-rate-limiter.cc
    ${GATEWAY_SRC_ROOT}/server/remote-config.cc
    ${GATEWAY_SRC_ROOT}/server/remote-gate.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-fan-out.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-notifier.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-stream-relay.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-stream-socket.cc
    ${GATEWAY_SRC_ROOT}/sync/fallback/fallback-log-repository.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-sync-source.cc
    ${GATEWAY_SRC_ROOT}/sync/notification-sync-source.cc
    ${GATEWAY_SRC_ROOT}/sync/notification-delivery-consumer.cc
    ${GATEWAY_SRC_ROOT}/shared/services/socket/nats-identity-change-sink.cc
    ${GATEWAY_SRC_ROOT}/shared/repositories/delivery-inbox/delivery-inbox-repository.cc
    ${GATEWAY_SRC_ROOT}/sync/productivity-sync-source.cc
    ${GATEWAY_SRC_ROOT}/sync/sync-fan-out.cc
    ${GATEWAY_SRC_ROOT}/sync/sync-registrar.cc
    ${GATEWAY_SRC_ROOT}/sync/voice-grpc-relay.cc
    ${GATEWAY_SRC_ROOT}/sync/user-change-fan-out.cc
)

target_include_directories(gateway-core PUBLIC ${GATEWAY_SRC_ROOT})

target_link_libraries(gateway-core PUBLIC argus_identity argus_sync
    argus::cert mdns::mdns argus::room argus::socket
    argus::client-voice argus::client-identity argus::client-camera
    argus::client-productivity argus::client-notification)
target_link_libraries(gateway-core PRIVATE contract::gateway)

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

target_link_libraries(gateway-test PRIVATE gateway-core doctest::doctest)

target_compile_options(gateway-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(gateway-test)

add_executable(audit-sync-read-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/audit-sync-read-test.cc)

target_include_directories(audit-sync-read-test PRIVATE ${GATEWAY_SRC_ROOT})

target_link_libraries(audit-sync-read-test PRIVATE gateway-core
                                                   doctest::doctest)

target_compile_options(audit-sync-read-test PRIVATE -Wall -Wextra)

add_executable(camera-notifier-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/camera-notifier-test.cc)

add_executable(notification-delivery-inbox-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/notification-delivery-inbox-test.cc)

target_include_directories(notification-delivery-inbox-test PRIVATE ${GATEWAY_SRC_ROOT})

target_compile_definitions(notification-delivery-inbox-test PRIVATE
    ARGUS_IDENTITY_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity/database/schema.sql")

target_link_libraries(notification-delivery-inbox-test PRIVATE gateway-core
                                                    doctest::doctest)

target_compile_options(notification-delivery-inbox-test PRIVATE -Wall -Wextra)

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
set_target_properties(audit-sync-read-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(camera-notifier-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
set_target_properties(notification-delivery-inbox-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME gateway-test COMMAND gateway-test)
add_test(NAME audit-sync-read-test COMMAND audit-sync-read-test)
add_test(NAME camera-notifier-test COMMAND camera-notifier-test)
add_test(NAME notification-delivery-inbox-test COMMAND notification-delivery-inbox-test)

# Opt-in live check against a real NATS; skips without ARGUS_NATS_URL.
add_executable(notification-delivery-live-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/notification-delivery-live-test.cc)

set_target_properties(notification-delivery-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-delivery-live-test PRIVATE ${GATEWAY_SRC_ROOT})

target_compile_definitions(notification-delivery-live-test PRIVATE
    ARGUS_IDENTITY_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity/database/schema.sql")

target_link_libraries(notification-delivery-live-test PRIVATE gateway-core
                                                    doctest::doctest)

target_compile_options(notification-delivery-live-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-live-test COMMAND notification-delivery-live-test)
