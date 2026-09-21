# validation: the DTO validation DSL (rules, macros, validator) over the
# shared response errors. Consumers link argus::validation.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_module(NAME validation
    SOURCES
        src/shared/validation/rules.hxx
        src/shared/validation/validation_dsl.hxx
        src/shared/validation/validator.hxx
    INCLUDES src
    DEPENDS argus::errors)

# Header-only: no compiled source sets the linker language on its own.
set_target_properties(argus_validation PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(argus_validation PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(validation-dsl-test tests/unit/validation-dsl-test.cc)
target_link_libraries(validation-dsl-test PRIVATE argus_validation
    doctest::doctest)
target_compile_options(validation-dsl-test PRIVATE -Wall -Wextra)
add_test(NAME validation-dsl-test COMMAND validation-dsl-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
