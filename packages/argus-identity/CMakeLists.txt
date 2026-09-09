cmake_minimum_required(VERSION 3.25)
project(argus-identity VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_identity: the identity service (auth/invitation/pairing/user
# features, the identity repositories and schemas, faces and portraits).
# Compiled INTO the argus-gateway binary for now — no new process, no new
# port; the later standalone extraction is a CMake/deploy change, not a
# second file move. The root project and every standalone consumer build
# share this one definition.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# Standalone configure (top level): add the sibling packages the identity
# module links, the vendored sqlite-vec the vec repositories ride and the
# ncnn build the face services compile against. Every guard keeps the root
# and consumer builds in charge. The empty cq-bridge stand-ins skip the
# bridge: standalone protobuf comes from the system, not from conan, so the
# bridge entry symbol would interpose libgrpc's own callbacks (see
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
    if(NOT TARGET argus::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-cert
                         ${CMAKE_BINARY_DIR}/argus-cert)
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
endif()

# The identity RPC contract (argus.identity.v1) this service serves.
if(NOT TARGET argus::sdk-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-contracts
                     ${CMAKE_BINARY_DIR}/argus-contracts EXCLUDE_FROM_ALL)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenCV CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

argus_module(NAME identity
    SOURCES
        src/feature/api/auth/controllers/auth-controller.cc
        src/feature/api/auth/dtos/create-device-login-dto.cc
        src/feature/api/auth/dtos/device-login-status-dto.cc
        src/feature/api/auth/dtos/login-dto.cc
        src/feature/api/auth/dtos/refresh-token-dto.cc
        src/feature/api/auth/dtos/register-dto.cc
        src/feature/api/auth/dtos/response-login-dto.cc
        src/feature/api/auth/dtos/response-refresh-token-dto.cc
        src/feature/api/auth/dtos/update-me-dto.cc
        src/feature/api/auth/services/auth-service.cc
        src/feature/api/invitation/controllers/invitation-controller.cc
        src/feature/api/invitation/dtos/create-invitation-dto.cc
        src/feature/api/invitation/dtos/resolve-invitation-dto.cc
        src/feature/api/invitation/dtos/response-invitation-dto.cc
        src/feature/api/invitation/dtos/response-invitation-resolve-dto.cc
        src/feature/api/invitation/services/invitation-feature-service.cc
        src/feature/api/pairing/controllers/pairing-controller.cc
        src/feature/api/pairing/dtos/pairing-dto.cc
        src/feature/api/pairing/dtos/response-pairing-dto.cc
        src/feature/rpc/identity-rpc.cc
        src/feature/api/user/controllers/portrait-preview-controller.cc
        src/feature/api/user/controllers/user-controller.cc
        src/feature/api/user/dtos/response-portrait-preview-capability-dto.cc
        src/feature/api/user/dtos/response-portrait-preview-image-dto.cc
        src/feature/api/user/dtos/update-user-dto.cc
        src/feature/api/user/services/portrait-preview-service.cc
        src/feature/api/user/services/user-feature-service.cc
        src/shared/repositories/device-credential/device-credential-repository.cc
        src/shared/repositories/device-login-challenge/device-login-challenge-repository.cc
        src/shared/repositories/face-embedding/face-embedding-repository.cc
        src/shared/repositories/person/person-repository.cc
        src/shared/repositories/portrait-preview-capability/portrait-preview-capability-repository.cc
        src/shared/repositories/refresh-token/refresh-token-repository.cc
        src/shared/repositories/stored-file/stored-file-repository.cc
        src/shared/repositories/user-invitation/user-invitation-repository.cc
        src/shared/repositories/user-portrait/user-portrait-repository.cc
        src/shared/repositories/user/user-repository.cc
        src/shared/schemas/device-credential/device-credential-schema.cc
        src/shared/schemas/device-login-challenge/device-login-challenge-schema.cc
        src/shared/schemas/face-embedding/face-embedding-schema.cc
        src/shared/schemas/person/person-schema.cc
        src/shared/schemas/portrait-preview-capability/portrait-preview-capability-schema.cc
        src/shared/schemas/refresh-token/refresh-token-schema.cc
        src/shared/schemas/stored-file/stored-file-schema.cc
        src/shared/schemas/user-invitation/user-invitation-schema.cc
        src/shared/schemas/user-portrait/user-portrait-schema.cc
        src/shared/schemas/user/user-schema.cc
        src/shared/services/face/face-db.cc
        src/shared/services/face/face-service.cc
        src/shared/services/storage/private-portrait-service.cc
    INCLUDES
        src
        ../../third_party
        ../../third_party/stb
    DEPENDS
        argus_common
        argus::auth
        argus::sdk-identity
        argus::audit
        argus::sqlite
        argus::cert
        argus::socket
        argus::hardware-profile-gpu
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        opencv::opencv
        OpenSSL::SSL
        OpenSSL::Crypto)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_identity PUBLIC ${ARGUS_NCNN_TARGET})
    target_include_directories(argus_identity SYSTEM PUBLIC
        ${CMAKE_BINARY_DIR}/third_party/ncnn/src)
endif()

target_compile_options(argus_identity PRIVATE -Wall -Wextra)

# The unit suites ride identity-migration: the root tree adds the tool
# folder EXCLUDE_FROM_ALL via its discovery glob, the standalone build adds
# it here (the CLI executable stays EXCLUDE_FROM_ALL on its own).
if(PROJECT_IS_TOP_LEVEL AND NOT TARGET identity-migration)
    add_subdirectory(tools/migrate-identity)
endif()

enable_testing()
find_package(doctest REQUIRED)

# The migration suite: schema apply + argus.db -> identity.db verified
# row by row, against the schema this folder owns.
add_executable(identity-migration-test tests/unit/identity-migration-test.cc)

target_link_libraries(identity-migration-test PRIVATE identity-migration
    doctest::doctest)

target_compile_options(identity-migration-test PRIVATE -Wall -Wextra)

add_test(NAME identity-migration-test COMMAND identity-migration-test)

# The device credential suite rides the identity domain: the DeviceFilter
# gate, the credential repository and the auth-service issuance flow.
add_executable(device-credential-test tests/unit/device-credential-test.cc)

target_link_libraries(device-credential-test PRIVATE argus_identity
    doctest::doctest)

target_compile_options(device-credential-test PRIVATE -Wall -Wextra)

add_test(NAME device-credential-test COMMAND device-credential-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
