include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME test-support
    HEADER_ONLY
    SOURCES
        src/test-support/app-runner.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

enable_testing()

add_test(NAME test-runner-lifetime-test
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check-runner-lifetime.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_test(NAME test-runner-lifetime-selftest
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check-runner-lifetime.py
            --self-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
