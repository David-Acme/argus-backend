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

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_contracts(NAME sync
    INCLUDES
        .
        src
    SOURCES
        src/shared/contracts/audit-log-priority.hxx
        src/shared/contracts/sync-operation.hxx
        src/shared/contracts/sync-filter.hxx
        src/shared/contracts/sync-limits.hxx
        src/shared/contracts/syncable.hxx
        src/shared/contracts/table-name.hxx
        src/shared/contracts/user-action.hxx
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

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
