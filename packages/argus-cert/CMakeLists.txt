cmake_minimum_required(VERSION 3.25)
project(argus-cert VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_cert: TLS certificate issuance for the instance's HTTPS listener.
# Consumers link argus::cert.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# Found before the sibling add_subdirectory block: the migration tool the
# sibling suites ride links Drogon from this scope.
find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

# Standalone configure (top level): the cert test rides argus_identity, so
# add the sibling packages identity and cert link, the vendored sqlite-vec
# and ncnn, and identity's migration tool the sibling suites ride. Identity
# lands after the ncnn block so its face services see the ncnn target. Every
# guard keeps the root and consumer builds in charge. The empty cq-bridge
# stand-ins skip the bridge: standalone protobuf comes from the system, so
# the bridge entry symbol would interpose libgrpc's own callbacks (see
# argus-contracts/CONTEXT.md).
if(PROJECT_IS_TOP_LEVEL)
    if(NOT TARGET argus_sdk_grpc_bridge_entry)
        add_library(argus_sdk_grpc_bridge_entry INTERFACE)
        add_library(argus_sdk_grpc_bridge_exit INTERFACE)
    endif()
    if(NOT TARGET argus_common)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                         ${CMAKE_BINARY_DIR}/argus-common)
    endif()
    if(NOT TARGET argus::room)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-room
                         ${CMAKE_BINARY_DIR}/argus-room)
    endif()
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
    if(NOT TARGET argus::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-sqlite
                         ${CMAKE_BINARY_DIR}/argus-sqlite)
    endif()
    if(NOT TARGET argus::socket)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-socket
                         ${CMAKE_BINARY_DIR}/argus-socket)
    endif()
    if(NOT TARGET argus::audit)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-audit
                         ${CMAKE_BINARY_DIR}/argus-audit)
    endif()
    if(NOT TARGET argus::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-auth
                         ${CMAKE_BINARY_DIR}/argus-auth)
    endif()

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
    if(NOT TARGET identity-migration)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-identity/tools/migrate-identity
                         ${CMAKE_BINARY_DIR}/argus-identity/tools/migrate-identity)
    endif()
    if(NOT TARGET argus::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-identity
                         ${CMAKE_BINARY_DIR}/argus-identity)
    endif()
endif()

argus_module(NAME cert
    SOURCES
        src/shared/services/cert/cert-service.cc
    INCLUDES src)

target_link_libraries(argus_cert
    PUBLIC
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_cert PRIVATE -Wall -Wextra)

# CertService rotation and the in-process TLS listener that proves the hot
# reload. It rides the identity domain, which is why it links argus_identity.
enable_testing()
find_package(doctest REQUIRED)

add_executable(cert-san-test tests/unit/cert-san-test.cc)
target_link_libraries(cert-san-test PRIVATE argus::cert argus_identity
    argus_common doctest::doctest)
target_compile_options(cert-san-test PRIVATE -Wall -Wextra)
add_test(NAME cert-san-test COMMAND cert-san-test)
