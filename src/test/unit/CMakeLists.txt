find_package(doctest REQUIRED)

set(UNIT_TESTS
    api-response-test
    enums-test
    role-access-test
    thread-budget-test
    validation-dsl-test
)

foreach(TEST_NAME IN LISTS UNIT_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cc)

    target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

if(TARGET doctest::doctest)
    get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(DOCTEST_INCLUDE_DIRS)
        set_target_properties(doctest::doctest PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
    endif()
endif()