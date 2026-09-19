# productivity-contract: the vocabulary that crosses the productivity
# boundary — a reminder detail's status, the access a shared calendar event
# carries and the error a membership check reports. Declared once, here, for
# the service that owns the boundary and for the packages and services that
# consume it. Header-only; consumers link contract::productivity.

cmake_minimum_required(VERSION 3.25)

add_library(contract-productivity INTERFACE)
add_library(contract::productivity ALIAS contract-productivity)
target_include_directories(contract-productivity INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(contract-productivity INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/membership-error.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/reminder-detail-status.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/share-access.hxx)

# The round-trip suite: every name is its own spelling and back.
enable_testing()
find_package(doctest REQUIRED)

add_executable(productivity-contract-vocabulary-test
    tests/unit/productivity-contract-vocabulary-test.cc)
target_link_libraries(productivity-contract-vocabulary-test PRIVATE
    contract-productivity doctest::doctest)
target_compile_options(productivity-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME productivity-contract-vocabulary-test
         COMMAND productivity-contract-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
