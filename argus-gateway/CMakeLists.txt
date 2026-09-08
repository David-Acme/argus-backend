cmake_minimum_required(VERSION 3.25)
project(argus-gateway VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GATEWAY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

if(NOT TARGET argus_identity)
    # Standalone build: reuse the root project's argus_common, argus_identity
    # and third-party sources so the gateway links the same objects as the
    # root-tree build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                     ${CMAKE_BINARY_DIR}/argus-common)

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-contracts
                     ${CMAKE_BINARY_DIR}/argus-contracts EXCLUDE_FROM_ALL)

    enable_language(C)
    add_library(sqlite-vec STATIC
                ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec/sqlite-vec.c)
    target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
    target_include_directories(sqlite-vec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec)
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
    set(NCNN_AVX2 ON CACHE BOOL "" FORCE)
    set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
    set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ncnn
                     ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    set(ARGUS_NCNN_TARGET ncnn)

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-cert
                     ${CMAKE_BINARY_DIR}/argus-cert)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-room
                     ${CMAKE_BINARY_DIR}/argus-room)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-sqlite
                     ${CMAKE_BINARY_DIR}/argus-sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-socket
                     ${CMAKE_BINARY_DIR}/argus-socket)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-audit
                     ${CMAKE_BINARY_DIR}/argus-audit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-auth
                     ${CMAKE_BINARY_DIR}/argus-auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-identity
                     ${CMAKE_BINARY_DIR}/argus-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-sync
                     ${CMAKE_BINARY_DIR}/argus-sync)
endif()

if(NOT TARGET argus::sdk-voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-contracts
                     ${CMAKE_BINARY_DIR}/argus-contracts EXCLUDE_FROM_ALL)
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
    ${GATEWAY_SRC_ROOT}/sync/camera-sync-source.cc
    ${GATEWAY_SRC_ROOT}/sync/sync-fan-out.cc
    ${GATEWAY_SRC_ROOT}/sync/sync-registrar.cc
    ${GATEWAY_SRC_ROOT}/sync/sync-relay.cc
    ${GATEWAY_SRC_ROOT}/sync/voice-grpc-relay.cc
    ${GATEWAY_SRC_ROOT}/sync/user-change-fan-out.cc
    ${GATEWAY_SRC_ROOT}/sync/user-change-sink.cc
)

target_include_directories(gateway-core PUBLIC ${GATEWAY_SRC_ROOT})

target_link_libraries(gateway-core PUBLIC argus_identity argus_sync
    argus::cert mdns::mdns argus::room argus::socket
    argus::sdk-voice argus::sdk-identity argus::sdk-camera)

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

target_include_directories(camera-notifier-test PRIVATE ${GATEWAY_SRC_ROOT})

target_link_libraries(camera-notifier-test PRIVATE gateway-core
                                                   doctest::doctest)

target_compile_options(camera-notifier-test PRIVATE -Wall -Wextra)

add_executable(push-intent-test
               ${CMAKE_CURRENT_SOURCE_DIR}/tests/push-intent-test.cc)

target_include_directories(push-intent-test PRIVATE ${GATEWAY_SRC_ROOT})

target_link_libraries(push-intent-test PRIVATE gateway-core doctest::doctest)

target_compile_options(push-intent-test PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# Test registration is standalone-only so the root project's ctest stays at
# its 38 backend cases.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_test(NAME gateway-test COMMAND gateway-test)
    add_test(NAME audit-sync-read-test COMMAND audit-sync-read-test)
    add_test(NAME camera-notifier-test COMMAND camera-notifier-test)
    add_test(NAME push-intent-test COMMAND push-intent-test)
endif()
