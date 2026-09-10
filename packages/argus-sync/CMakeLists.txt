cmake_minimum_required(VERSION 3.25)
project(argus-sync VERSION 1.0.0 LANGUAGES CXX)

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

# Standalone configure (top level): the sync test rides argus_identity, so
# add the sibling packages sync and identity link, the vendored sqlite-vec
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
    if(NOT TARGET argus::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-cert
                         ${CMAKE_BINARY_DIR}/argus-cert)
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

set(ARGUS_SYNC_SOURCES
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
)

add_library(argus_sync STATIC ${ARGUS_SYNC_SOURCES})

target_include_directories(argus_sync
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)

target_link_libraries(argus_sync
    PUBLIC
        argus_identity
        argus::audit
        argus::socket
        argus::room
        argus_common
        Drogon::Drogon)

target_compile_options(argus_sync PRIVATE -Wall -Wextra)

# The voice half of the media forwarder left with F6-3: the whole former
# argus_sync_media library is argus-voice's voice-core now; the gateway relays
# the frames to the owner service instead of linking any of this.

# The sync suites: the empty-table contract and the golden /sync transcript
# whose fixtures are frozen (a re-record is a contract change, not a fix).
enable_testing()
find_package(doctest REQUIRED)

add_executable(event-sync-empty-test tests/unit/event-sync-empty-test.cc)
target_link_libraries(event-sync-empty-test PRIVATE argus_sync
    argus::sqlite argus_common Drogon::Drogon sqlite-vec doctest::doctest)
target_compile_options(event-sync-empty-test PRIVATE -Wall -Wextra)
add_test(NAME event-sync-empty-test COMMAND event-sync-empty-test)

add_executable(golden-sync-test tests/e2e/golden-sync-test.cc)
target_compile_definitions(golden-sync-test PRIVATE
    ARGUS_TEST_SYNC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sync")
target_link_libraries(golden-sync-test PRIVATE Drogon::Drogon)
target_compile_options(golden-sync-test PRIVATE -Wall -Wextra)
add_test(NAME golden-sync-test COMMAND golden-sync-test)
