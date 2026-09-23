cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME identity
    INCLUDES src
    SOURCES
        src/identity/identity-errors.hxx
    DEPENDS argus::lib::errors)

enable_testing()
find_package(doctest REQUIRED)

add_executable(identity-contract-catalog-test
    tests/unit/identity-contract-catalog-test.cc)
target_link_libraries(identity-contract-catalog-test PRIVATE
    argus::contracts::identity doctest::doctest)
target_compile_options(identity-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME identity-contract-catalog-test
         COMMAND identity-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
