cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_contracts(NAME camera
    INCLUDES src
    SOURCES
        src/camera/camera-driver.hxx
        src/camera/camera-errors.hxx
        src/camera/camera-record-mode.hxx
        src/camera/camera-row-projection.hxx
        src/camera/event-severity.hxx
        src/camera/identity-state.hxx
        src/camera/zone-type.hxx
    DEPENDS argus::lib::errors Drogon::Drogon)

enable_testing()
find_package(doctest REQUIRED)

add_executable(camera-contract-vocabulary-test
    tests/unit/camera-contract-vocabulary-test.cc)
target_link_libraries(camera-contract-vocabulary-test PRIVATE
    argus::contracts::camera doctest::doctest)
target_compile_options(camera-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME camera-contract-vocabulary-test COMMAND camera-contract-vocabulary-test)

add_executable(camera-contract-catalog-test
    tests/unit/camera-contract-catalog-test.cc)
target_link_libraries(camera-contract-catalog-test PRIVATE
    argus::contracts::camera doctest::doctest)
target_compile_options(camera-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME camera-contract-catalog-test COMMAND camera-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
