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
        src/sync/stream-retention.hxx
        src/sync/audit-retention.hxx
        src/sync/module-emit.hxx
        src/sync/user-change-sink.hxx
        src/sync/camera-change-sink.hxx
        src/sync/user-audit-event.hxx
        src/sync/module-audit-event.hxx
        src/sync/user-action-event.hxx
        src/sync/auth-change-sink.hxx
        src/sync/identity-change-sink.hxx
        src/sync/sync-control-sink.hxx
        src/sync/sync-forwarder.hxx
    DEPENDS
        argus::lib::errors
        argus::lib::text
        Drogon::Drogon)

enable_testing()
find_package(doctest REQUIRED)

add_executable(sync-contract-vocabulary-test
    tests/unit/sync-contract-vocabulary-test.cc)
target_link_libraries(sync-contract-vocabulary-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(sync-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME sync-contract-vocabulary-test COMMAND sync-contract-vocabulary-test)

add_executable(sync-contract-catalog-test
    tests/unit/sync-contract-catalog-test.cc)
target_link_libraries(sync-contract-catalog-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(sync-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME sync-contract-catalog-test COMMAND sync-contract-catalog-test)

add_executable(sync-change-test tests/unit/sync-change-test.cc)
target_link_libraries(sync-change-test PRIVATE
    argus::contracts::sync
    argus::contracts::auth
    argus::lib::text
    argus::lib::nats
    doctest::doctest)
target_compile_options(sync-change-test PRIVATE -Wall -Wextra)
add_test(NAME sync-change-test COMMAND sync-change-test)

add_executable(user-action-event-test tests/unit/user-action-event-test.cc)
target_link_libraries(user-action-event-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(user-action-event-test PRIVATE -Wall -Wextra)
add_test(NAME user-action-event-test COMMAND user-action-event-test)

add_executable(stream-retention-test tests/unit/stream-retention-test.cc)
target_link_libraries(stream-retention-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(stream-retention-test PRIVATE -Wall -Wextra)
add_test(NAME stream-retention-test COMMAND stream-retention-test)

add_executable(audit-retention-test tests/unit/audit-retention-test.cc)
target_link_libraries(audit-retention-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(audit-retention-test PRIVATE -Wall -Wextra)
add_test(NAME audit-retention-test COMMAND audit-retention-test)

add_executable(sync-scope-query-test tests/unit/sync-scope-query-test.cc)
target_link_libraries(sync-scope-query-test PRIVATE
    argus::contracts::sync doctest::doctest)
target_compile_options(sync-scope-query-test PRIVATE -Wall -Wextra)
add_test(NAME sync-scope-query-test COMMAND sync-scope-query-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
