cmake_minimum_required(VERSION 3.25)
project(argus-sync VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_sync: the sync WebSocket surface (socket, native sync-table service, the
# event repository and the pull-source contracts the host implements) plus the
# three audit tables it alone writes and the room registry its fan-out reads.
# The legacy backend and the argus-gateway both consume this one definition; the
# gateway never pulls the media objects because nothing it links references
# them. The cross-domain repositories this module used to carry are their
# owners' now (sub-step 3a-1b): the camera, productivity and notification
# services compile their own tables, and this module keeps the engine.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# Found before the sibling add_subdirectory block: the migration tool the
# sibling suites ride links Drogon from this scope.
find_package(Drogon CONFIG REQUIRED)

# The leaves this module links: a DAG, so their guards carry them into every
# build. The sibling packages stay behind PROJECT_IS_TOP_LEVEL because
# argus_sync rides argus_identity and its suite: only the project at the top
# of the configure provides those.
if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::contracts::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/camera
                     ${CMAKE_BINARY_DIR}/contracts/camera)
endif()

if(NOT TARGET argus::contracts::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/notification
                     ${CMAKE_BINARY_DIR}/contracts/notification)
endif()

if(NOT TARGET argus::contracts::productivity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/productivity
                     ${CMAKE_BINARY_DIR}/contracts/productivity)
endif()

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/text
                     ${CMAKE_BINARY_DIR}/text)
endif()
if(NOT TARGET argus::lib::nats)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/nats
                     ${CMAKE_BINARY_DIR}/nats)
endif()
if(NOT TARGET argus::lib::validation)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/validation
                     ${CMAKE_BINARY_DIR}/validation)
endif()
if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/errors
                     ${CMAKE_BINARY_DIR}/errors)
endif()

# Standalone configure (top level): the sync test rides argus_identity, so
# add the sibling packages sync and identity link, the vendored sqlite-vec
# and ncnn, and identity's migration tool the sibling suites ride. Identity
# lands after the ncnn block so its face services see the ncnn target.
if(PROJECT_IS_TOP_LEVEL)
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
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
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../identity/tools/migrate-identity
                         ${CMAKE_BINARY_DIR}/identity/tools/migrate-identity)
    endif()
    if(NOT TARGET argus::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../identity
                         ${CMAKE_BINARY_DIR}/identity)
    endif()
endif()

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

set(ARGUS_SYNC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/socket/sync/socket/sync-forwarder.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/socket/sync/socket/sync-socket.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/socket/sync/services/sync-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/feature/socket/sync/services/synchronized-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/audit-log/audit-log-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/event/event-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/user-action-log/user-action-log-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/user-audit-log/user-audit-log-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/audit-log/audit-log-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/event/event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/person-event/person-event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/user-action-log/user-action-log-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/user-audit-log/user-audit-log-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/services/audit-log/audit-log-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/services/room/room-manager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/services/user-audit-log/user-audit-log-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/camera-sync-source.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/productivity-sync-source.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/notification-sync-source.hxx
)

add_library(argus_sync STATIC ${ARGUS_SYNC_SOURCES})

target_include_directories(argus_sync
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)

target_link_libraries(argus_sync PUBLIC
    argus_identity
    argus::lib::auth
    argus::contracts::sync
    argus::contracts::auth
    argus::contracts::camera
    argus::contracts::notification
    argus::lib::sqlite
    argus::lib::text
    argus::lib::nats
    argus::lib::validation
    argus::lib::errors
    Drogon::Drogon)

target_compile_options(argus_sync PRIVATE -Wall -Wextra)

# The voice half of the media forwarder left with F6-3: the whole former
# argus_sync_media library is argus-voice's voice-core now; the gateway relays
# the frames to the owner service instead of linking any of this.

# The suites went with the surface (sub-step 3a-1c): services/sync owns the
# five that exercise the socket, the fan-out, the audit reads and the delivery
# inbox, and their fixtures. This package compiles the engine only, until
# sub-step d moves it too.
