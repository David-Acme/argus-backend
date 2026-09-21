cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME camera
    INCLUDES .
    SOURCES
        camera-driver.hxx
        camera-errors.hxx
        camera-record-mode.hxx
        event-severity.hxx
        zone-type.hxx
    DEPENDS argus::lib::errors)

# The round-trip suite: every name is its own spelling and back.
enable_testing()
find_package(doctest REQUIRED)

add_executable(camera-contract-vocabulary-test
    tests/unit/camera-contract-vocabulary-test.cc)
target_link_libraries(camera-contract-vocabulary-test PRIVATE
    argus::contracts::camera doctest::doctest)
target_compile_options(camera-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME camera-contract-vocabulary-test COMMAND camera-contract-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
