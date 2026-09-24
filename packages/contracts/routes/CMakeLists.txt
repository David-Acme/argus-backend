cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts(NAME routes
    INCLUDES src
    SOURCES
        src/routes/service-discovery.hxx)

enable_testing()
find_package(doctest REQUIRED)

add_executable(route-discovery-test tests/unit/route-discovery-test.cc)
target_link_libraries(route-discovery-test PRIVATE
    argus::contracts::routes
    doctest::doctest)
target_compile_options(route-discovery-test PRIVATE -Wall -Wextra)
add_test(NAME route-discovery-test COMMAND route-discovery-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
