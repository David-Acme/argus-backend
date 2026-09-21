# errors: the refusal vocabulary -- the code enum, the definition that pairs a
# code with its status and message, and the typed exceptions that carry it
# (response and validation). No Drogon, no HTTP type, no JSON: a pure leaf, so
# a domain contract can declare its errors without dragging a framework in.
# Consumers link argus::lib::errors.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_lib(NAME errors
    SOURCES
        src/errors/response-exception.cc
        src/errors/error-code.hxx
        src/errors/error-definition.hxx
        src/errors/response-exception.hxx
        src/errors/validation-exception.hxx
    INCLUDES src)

target_compile_options(argus_lib_errors PRIVATE -Wall -Wextra)

# The vocabulary's own suite: every code's wire string, and the three shapes a
# refusal can take. Definition-backed errors are asserted in the contracts that
# declare them; this package tests the mechanism.
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
