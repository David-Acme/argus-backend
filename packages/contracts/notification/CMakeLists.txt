cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME notification
    INCLUDES src
    SOURCES
        src/notification/notification-errors.hxx
        src/notification/notification-delivery-sink.hxx
        src/notification/notification-delivery-status.hxx
    DEPENDS
        argus::lib::errors)

enable_testing()
find_package(doctest REQUIRED)

add_executable(notification-contract-vocabulary-test
    tests/unit/notification-contract-vocabulary-test.cc)
target_link_libraries(notification-contract-vocabulary-test PRIVATE
    argus::contracts::notification doctest::doctest)
target_compile_options(notification-contract-vocabulary-test PRIVATE
    -Wall -Wextra)
add_test(NAME notification-contract-vocabulary-test
         COMMAND notification-contract-vocabulary-test)

add_executable(notification-contract-catalog-test
    tests/unit/notification-contract-catalog-test.cc)
target_link_libraries(notification-contract-catalog-test PRIVATE
    argus::contracts::notification doctest::doctest)
target_compile_options(notification-contract-catalog-test PRIVATE
    -Wall -Wextra)
add_test(NAME notification-contract-catalog-test
         COMMAND notification-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
