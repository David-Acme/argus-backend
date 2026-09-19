cmake_minimum_required(VERSION 3.25)

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

if(NOT TARGET argus::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

add_library(contract-sync INTERFACE)
add_library(contract::sync ALIAS contract-sync)
target_include_directories(contract-sync INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_sources(contract-sync INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/audit-log-priority.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/sync-operation.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/sync-filter.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/syncable.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/table-name.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/user-action.hxx)
target_link_libraries(contract-sync INTERFACE pkg::response
    argus::text Drogon::Drogon)

# The two enums the old enums-test covered that moved here: TableName and
# UserAction. SyncOperation has helpers but no round-trip case — the old suite
# never had one either — and AuditLogPriority has no helpers to round-trip.
enable_testing()
find_package(doctest REQUIRED)

add_executable(sync-contract-vocabulary-test
    tests/unit/sync-contract-vocabulary-test.cc)
target_link_libraries(sync-contract-vocabulary-test PRIVATE
    contract-sync doctest::doctest)
target_compile_options(sync-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME sync-contract-vocabulary-test COMMAND sync-contract-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
