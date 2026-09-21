cmake_minimum_required(VERSION 3.25)

if(NOT TARGET argus::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

add_library(contract-camera INTERFACE)
add_library(contract::camera ALIAS contract-camera)
target_include_directories(contract-camera INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(contract-camera INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/camera-driver.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/camera-errors.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/camera-record-mode.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/event-severity.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/zone-type.hxx)
target_link_libraries(contract-camera INTERFACE argus::errors)

# The round-trip suite: every name is its own spelling and back.
enable_testing()
find_package(doctest REQUIRED)

add_executable(camera-contract-vocabulary-test
    tests/unit/camera-contract-vocabulary-test.cc)
target_link_libraries(camera-contract-vocabulary-test PRIVATE
    contract-camera doctest::doctest)
target_compile_options(camera-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME camera-contract-vocabulary-test COMMAND camera-contract-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
