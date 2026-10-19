cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts(NAME push
    INCLUDES src
    SOURCES
        src/push/push-provider.hxx
        src/push/push-kind.hxx
        src/push/push-priority.hxx
        src/push/push-outcome.hxx
        src/push/push-envelope.hxx
    DEPENDS)

if(ARGUS_PACKAGE_TESTS)
    enable_testing()
    find_package(doctest REQUIRED)

    add_executable(push-contract-test tests/unit/push-contract-test.cc)
    target_link_libraries(push-contract-test PRIVATE
        argus::contracts::push argus::lib::doctest-main)
    target_compile_options(push-contract-test PRIVATE -Wall -Wextra)
    add_test(NAME push-contract-test COMMAND push-contract-test)

    get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(DOCTEST_INCLUDE_DIRS)
        set_target_properties(doctest::doctest PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
    endif()
endif()
