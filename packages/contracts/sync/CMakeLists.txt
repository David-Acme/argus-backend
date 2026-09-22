cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

# Only the change-payload suite needs these two: it pins the role names through
# the enum's own spelling and the subject the payloads travel on.
if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../auth
                     ${CMAKE_CURRENT_BINARY_DIR}/auth)
endif()

if(NOT TARGET argus::lib::nats)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/nats
                     ${CMAKE_CURRENT_BINARY_DIR}/nats)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_contracts(NAME sync
    INCLUDES
        src
    SOURCES
        src/sync/sync-errors.hxx
        src/sync/audit-log-priority.hxx
        src/sync/sync-operation.hxx
        src/sync/sync-filter.hxx
        src/sync/sync-limits.hxx
        src/sync/syncable.hxx
        src/sync/table-name.hxx
        src/sync/role-permission.hxx
        src/sync/user-action.hxx
        src/sync/socket-emit-dto.hxx
        src/sync/sync-change.hxx
        src/sync/user-change-sink.hxx
        src/sync/camera-change-sink.hxx
        src/sync/user-audit-event.hxx
        src/sync/module-audit-event.hxx
        src/sync/user-action-event.hxx
        src/sync/identity-change-sink.hxx
        src/sync/sync-control-sink.hxx
    DEPENDS
        argus::lib::errors
        argus::lib::text
        Drogon::Drogon)

# The two enums the old enums-test covered that moved here: TableName and
# UserAction. SyncOperation has helpers but no round-trip case — the old suite
# never had one either — and AuditLogPriority has no helpers to round-trip.
enable_testing()
find_package(doctest REQUIRED)

add_executable(sync-contract-vocabulary-test
    tests/unit/sync-contract-vocabulary-test.cc)
target_link_libraries(sync-contract-vocabulary-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(sync-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME sync-contract-vocabulary-test COMMAND sync-contract-vocabulary-test)

# The catalog suite: the six refusals the sync boundary throws, pinned as a
# table and checked against what the response serializer accepts.
add_executable(sync-contract-catalog-test
    tests/unit/sync-contract-catalog-test.cc)
target_link_libraries(sync-contract-catalog-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(sync-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME sync-contract-catalog-test COMMAND sync-contract-catalog-test)

# The change-payload suite: the SocketEmitDto triple, the room-control actions
# and the subject they travel on, moved here from packages/socket with the
# vocabulary.
add_executable(sync-change-test tests/unit/sync-change-test.cc)
target_link_libraries(sync-change-test PRIVATE
    argus::contracts::sync
    argus::contracts::auth
    argus::lib::text
    argus::lib::nats
    doctest::doctest)
target_compile_options(sync-change-test PRIVATE -Wall -Wextra)
add_test(NAME sync-change-test COMMAND sync-change-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
