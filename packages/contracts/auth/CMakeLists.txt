# auth-contract: the vocabulary that crosses the auth boundary — UserRole, the
# role every JWT, DTO and role gate names. Declared once, here, for the service
# that owns the auth boundary and for the packages and services that consume
# it. Header-only; consumers link argus::contracts::auth.

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME auth
    INCLUDES src
    SOURCES
        src/auth/auth-errors.hxx
        src/auth/request-context.hxx
        src/auth/user-role.hxx
    DEPENDS argus::lib::errors)

# The round-trip suite: every name is its own spelling and back.
enable_testing()
find_package(doctest REQUIRED)

add_executable(auth-contract-vocabulary-test
    tests/unit/auth-contract-vocabulary-test.cc)
target_link_libraries(auth-contract-vocabulary-test PRIVATE
    argus::contracts::auth doctest::doctest)
target_compile_options(auth-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME auth-contract-vocabulary-test COMMAND auth-contract-vocabulary-test)

# The catalog suite: the four refusals the auth boundary throws, pinned as a
# table and checked against what the response serializer accepts.
add_executable(auth-contract-catalog-test
    tests/unit/auth-contract-catalog-test.cc)
target_link_libraries(auth-contract-catalog-test PRIVATE
    argus::contracts::auth doctest::doctest)
target_compile_options(auth-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME auth-contract-catalog-test COMMAND auth-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
