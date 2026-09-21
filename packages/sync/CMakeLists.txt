cmake_minimum_required(VERSION 3.25)
project(argus-sync VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_sync: the sync WebSocket surface (socket, native sync-table service and
# the sync-path repositories). The legacy backend and the argus-gateway both
# consume this one definition; the gateway never pulls the media objects
# because nothing it links references them.

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
# lands after the ncnn block so its face services see the ncnn target. The
# empty cq-bridge stand-ins skip the bridge: standalone protobuf comes from
# the system, so the bridge entry symbol would interpose libgrpc's own
# callbacks (see packages/contracts/CONTEXT.md).
if(PROJECT_IS_TOP_LEVEL)
    if(NOT TARGET argus_client_grpc_bridge_entry)
        add_library(argus_client_grpc_bridge_entry INTERFACE)
        add_library(argus_client_grpc_bridge_exit INTERFACE)
    endif()
    if(NOT TARGET argus::room)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../room
                         ${CMAKE_BINARY_DIR}/room)
    endif()
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::socket)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../socket
                         ${CMAKE_BINARY_DIR}/socket)
    endif()
    if(NOT TARGET argus::audit)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../audit
                         ${CMAKE_BINARY_DIR}/audit)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/camera/camera-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/camera-stream/camera-stream-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/zone/zone-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/reminder/reminder-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/reminder-detail/reminder-detail-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/calendar-event/calendar-event-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/calendar-event-share/calendar-event-share-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/project/project-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/project-member/project-member-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/project-task/project-task-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/event/event-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/repositories/notification/notification-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/camera/camera-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/camera-stream/camera-stream-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/zone/zone-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/reminder/reminder-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/reminder-detail/reminder-detail-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/calendar-event/calendar-event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/calendar-event-share/calendar-event-share-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/project/project-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/project-member/project-member-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/project-task/project-task-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/event/event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/person-event/person-event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/schemas/notification/notification-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/services/notification/notification-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/camera-sync-source.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/productivity-sync-source.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/notification-sync-source.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/notification-delivery-sink.hxx
)

add_library(argus_sync STATIC ${ARGUS_SYNC_SOURCES})

target_include_directories(argus_sync
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)

target_link_libraries(argus_sync PUBLIC
    argus_identity
    argus::audit
    argus::socket
    argus::room
    argus::lib::auth
    argus::contracts::sync
    argus::contracts::auth
    argus::contracts::camera
    argus::contracts::notification
    argus::contracts::productivity
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::errors
    Drogon::Drogon)

target_compile_options(argus_sync PRIVATE -Wall -Wextra)

# The voice half of the media forwarder left with F6-3: the whole former
# argus_sync_media library is argus-voice's voice-core now; the gateway relays
# the frames to the owner service instead of linking any of this.

# The sync suites: the empty-table contract and the golden /sync transcript
# whose fixtures are frozen (a re-record is a contract change, not a fix).
find_package(doctest REQUIRED)

add_executable(event-sync-empty-test tests/unit/event-sync-empty-test.cc)
target_link_libraries(event-sync-empty-test PRIVATE
    argus_sync
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    sqlite-vec
    doctest::doctest)
target_compile_options(event-sync-empty-test PRIVATE -Wall -Wextra)
add_test(NAME event-sync-empty-test COMMAND event-sync-empty-test)

add_executable(golden-sync-test tests/e2e/golden-sync-test.cc)
target_compile_definitions(golden-sync-test PRIVATE
    ARGUS_TEST_SYNC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sync")
target_link_libraries(golden-sync-test PRIVATE Drogon::Drogon)
target_compile_options(golden-sync-test PRIVATE -Wall -Wextra)
add_test(NAME golden-sync-test COMMAND golden-sync-test)
