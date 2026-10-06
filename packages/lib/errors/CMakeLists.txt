include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_lib(NAME errors
    SOURCES
        src/errors/response-exception.cc
        src/errors/error-code.hxx
        src/errors/error-definition.hxx
        src/errors/error-list.hxx
        src/errors/response-exception.hxx
        src/errors/validation-exception.hxx
    INCLUDES src)

target_compile_options(argus_lib_errors PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(error-definition-test tests/unit/error-definition-test.cc)
target_link_libraries(error-definition-test PRIVATE argus::lib::errors doctest::doctest)
target_compile_options(error-definition-test PRIVATE -Wall -Wextra)

add_test(NAME error-definition-test COMMAND error-definition-test)
