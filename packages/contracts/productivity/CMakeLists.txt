cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME productivity
    INCLUDES src
    SOURCES
        src/productivity/membership-error.hxx
        src/productivity/productivity-errors.hxx
        src/productivity/reminder-detail-status.hxx
        src/productivity/share-access.hxx
    DEPENDS argus::lib::errors)

enable_testing()
find_package(doctest REQUIRED)

add_executable(productivity-contract-vocabulary-test
    tests/unit/productivity-contract-vocabulary-test.cc)
target_link_libraries(productivity-contract-vocabulary-test PRIVATE
    argus::contracts::productivity doctest::doctest)
target_compile_options(productivity-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME productivity-contract-vocabulary-test
         COMMAND productivity-contract-vocabulary-test)

add_executable(productivity-contract-catalog-test
    tests/unit/productivity-contract-catalog-test.cc)
target_link_libraries(productivity-contract-catalog-test PRIVATE
    argus::contracts::productivity doctest::doctest)
target_compile_options(productivity-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME productivity-contract-catalog-test
         COMMAND productivity-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
