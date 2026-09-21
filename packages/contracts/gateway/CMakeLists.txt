cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME gateway
    INCLUDES src
    SOURCES
        src/gateway/gateway-errors.hxx
    DEPENDS argus::lib::errors)

# The catalog suite: the nine refusals the gateway's gates throw, pinned as a
# table and checked against what the response serializer will accept.
enable_testing()
find_package(doctest REQUIRED)

add_executable(gateway-contract-catalog-test
    tests/unit/gateway-contract-catalog-test.cc)
target_link_libraries(gateway-contract-catalog-test PRIVATE
    argus::contracts::gateway doctest::doctest)
target_compile_options(gateway-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME gateway-contract-catalog-test COMMAND gateway-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
