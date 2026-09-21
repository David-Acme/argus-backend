# validation: the DTO validation DSL (rules, macros, validator) over the
# shared response errors. Header-only, so the target is INTERFACE; consumers
# link argus::lib::validation.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_lib(NAME validation
    HEADER_ONLY
    SOURCES
        src/validation/details/rules.hxx
        src/validation/validation_dsl.hxx
        src/validation/validator.hxx
    INCLUDES src
    DEPENDS argus::lib::errors)

enable_testing()
find_package(doctest REQUIRED)

add_executable(validation-dsl-test tests/unit/validation-dsl-test.cc)
target_link_libraries(validation-dsl-test PRIVATE argus::lib::validation
    doctest::doctest)
target_compile_options(validation-dsl-test PRIVATE -Wall -Wextra)
add_test(NAME validation-dsl-test COMMAND validation-dsl-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
